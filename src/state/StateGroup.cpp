#include "tanh/state/StateGroup.h"
#include "tanh/state/State.h"

namespace thl {

// StateGroup state management methods
void StateGroup::clear_groups() {
    // First, collect all groups to clear (to avoid recursive RCU updates)
    std::vector<std::shared_ptr<StateGroup>> groups_to_clear;

    m_groups_rcu.read([&](const GroupMap& groups) {
        groups_to_clear.reserve(groups.size());
        for (const auto& [name, group] : groups) { groups_to_clear.push_back(group); }
    });

    // Clear all subgroups outside of RCU context
    for (auto& group : groups_to_clear) { group->clear(); }

    // Now clear the groups map
    m_groups_rcu.update([&](GroupMap& groups) { groups.clear(); });
}

void StateGroup::clear() {
    // Clear parameters in this group
    if (m_parent == nullptr) {
        // If this is the root state, clear parameters - already thread safe
        m_root_state->clear();
    } else {
        // Get the full path once to avoid repeated calls
        std::string_view full_path = get_full_path();

        // Collect keys to delete from storage under mutex
        std::vector<std::string> keys_to_delete;
        {
            std::scoped_lock lock(m_root_state->m_storage_mutex);
            for (const auto& [key, record] : m_root_state->m_storage) {
                if (key.starts_with(full_path)) { keys_to_delete.push_back(key); }
            }
        }

        // Remove from the lock-free index first so readers can no longer
        // obtain pointers to records we are about to destroy.
        m_root_state->m_index_rcu.update([&](auto& idx) {
            for (const auto& key : keys_to_delete) { idx.erase(key); }
        });

        // Now safe to destroy the records from storage
        {
            std::scoped_lock lock(m_root_state->m_storage_mutex);
            for (const auto& key : keys_to_delete) { m_root_state->m_storage.erase(key); }
        }
    }
    clear_groups();
}

bool StateGroup::is_empty() const TANH_NONBLOCKING_FUNCTION {
    // Check if we have any subgroups using RCU (lock-free read)
    bool has_groups = false;
    m_groups_rcu.read([&](const GroupMap& groups) { has_groups = !groups.empty(); });

    if (has_groups) { return false; }

    // Get the full path once to avoid repeated calls - real-time safe
    std::string_view full_path = get_full_path();

    // Use RCU index to check if this group has any parameters (lock-free read)
    bool has_parameters = false;
    m_root_state->m_index_rcu.read([&](const auto& idx) {
        for (const auto& [key, record] : idx) {
            if (key.find(full_path) == 0) {
                has_parameters = true;
                return;  // Early exit from lambda
            }
        }
    });

    return !has_parameters;
}

// StateGroup listener management
void StateGroup::add_listener(ParameterListener* listener) {
    m_listeners_rcu.update([&](ListenerData& data) {
        // Check if listener already exists
        auto it = std::ranges::find(data.m_object_listeners, listener);
        if (it == data.m_object_listeners.end()) { data.m_object_listeners.push_back(listener); }
    });
}

void StateGroup::remove_listener(ParameterListener* listener) {
    m_listeners_rcu.update([&](ListenerData& data) {
        auto it = std::ranges::find(data.m_object_listeners, listener);
        if (it != data.m_object_listeners.end()) { data.m_object_listeners.erase(it); }
    });
}

size_t StateGroup::add_callback_listener(ParameterChangeCallback callback) {
    size_t id = 0;
    m_listeners_rcu.update([&](ListenerData& data) {
        id = data.m_next_listener_id++;
        data.m_callback_listeners[id] = std::move(callback);
    });
    return id;
}

void StateGroup::remove_callback_listener(size_t listener_id) {
    m_listeners_rcu.update(
        [&](ListenerData& data) { data.m_callback_listeners.erase(listener_id); });
}

void StateGroup::notify_parameter_change(std::string_view path) {
    // Resolve the path to get the parameter
    auto [group, param_name] = resolve_path(path);
    if (!group || !group->m_root_state) { return; }

    // Ensure thread is registered for RCU and buffers
    m_root_state->ensure_thread_registered();

    try {
        // Get the parameter
        // Use State's pre-allocated buffer
        m_root_state->m_temp_buffer_1().clear();

        std::string_view group_path = group->get_full_path();
        detail::join_path(group_path, param_name, m_root_state->m_temp_buffer_1());
        Parameter param = group->m_root_state->get_from_root(m_root_state->m_temp_buffer_1());

        // Look up gesture state
        bool is_in_gesture = false;
        m_root_state->m_index_rcu.read([&](const auto& idx) {
            auto it = idx.find(m_root_state->m_temp_buffer_1());
            if (it != idx.end()) {
                is_in_gesture = it->second->m_metadata.m_in_gesture.load(std::memory_order_relaxed);
            }
        });

        // Notify all listeners
        notify_listeners(path, param, NotifyStrategies::All, nullptr, is_in_gesture);
    } catch (const StateKeyNotFoundException&) {  // NOLINT(bugprone-empty-catch) key may not exist
    }
}

void StateGroup::notify_listeners(std::string_view path,
                                  const Parameter& param,
                                  NotifyStrategies strategy,
                                  ParameterListener* source,
                                  bool in_gesture) const {
    if (strategy == NotifyStrategies::None) { return; }
    // Notify listeners at this level using RCU (lock-free read)
    m_listeners_rcu.read([&](const ListenerData& data) {
        // Notify object-based listeners
        for (auto listener : data.m_object_listeners) {
            if ((strategy == NotifyStrategies::Others && listener == source) ||
                (strategy == NotifyStrategies::Self && listener != source)) {
                continue;
            }
            // Skip listeners that don't want notifications during gestures
            if (in_gesture && !listener->m_receives_during_gesture) { continue; }
            listener->on_parameter_changed(path, param);
        }

        // Notify callback-based listeners
        if (strategy != NotifyStrategies::Self) {
            for (const auto& [id, callback] : data.m_callback_listeners) { callback(path, param); }
        }
    });

    // Then, notify parent groups to propagate up the hierarchy
    if (m_parent) { m_parent->notify_listeners(path, param, strategy, source, in_gesture); }
}

// StateGroup implementation
StateGroup::StateGroup(State* root_state, StateGroup* parent, std::string_view name)
    : m_root_state(root_state), m_parent(parent), m_name(name) {}

StateGroup::~StateGroup() {
    t_registered_states().erase(this);
}

// Group management
StateGroup* StateGroup::create_group(std::string_view name) {
    std::string name_str(name);

    // First, check if group already exists
    StateGroup* existing_group = nullptr;
    m_groups_rcu.read([&](const GroupMap& groups) {
        auto it = groups.find(name_str);
        if (it != groups.end()) { existing_group = it->second.get(); }
    });

    if (existing_group) { return existing_group; }

    // Create new group and add it to the map
    auto new_group = std::make_shared<StateGroup>(m_root_state, this, name);
    auto* group_ptr = new_group.get();
    group_ptr->ensure_thread_registered();

    m_groups_rcu.update([&](GroupMap& groups) { groups[name_str] = new_group; });

    return group_ptr;
}

StateGroup* StateGroup::get_group(std::string_view name) const TANH_NONBLOCKING_FUNCTION {
    std::string name_str(name);
    StateGroup* found_group = nullptr;

    m_groups_rcu.read([&](const GroupMap& groups) {
        auto it = groups.find(name_str);
        if (it != groups.end()) { found_group = it->second.get(); }
    });

    return found_group;
}

bool StateGroup::has_group(std::string_view name) const TANH_NONBLOCKING_FUNCTION {
    std::string name_str(name);
    bool found = false;

    m_groups_rcu.read(
        [&](const GroupMap& groups) { found = groups.find(name_str) != groups.end(); });

    return found;
}

// Get the full path of this group
std::string_view StateGroup::get_full_path() const TANH_NONBLOCKING_FUNCTION {
    if (!m_parent || m_parent == m_root_state) { return m_name; }

    // Ensure thread is registered for RCU and buffers
    m_root_state->ensure_thread_registered();

    // Use State's pre-allocated buffer
    m_root_state->m_temp_buffer_1().clear();

    // Build path iteratively by calculating depth first, then building from
    // root Count the depth to avoid recursion
    int depth = 0;
    const StateGroup* current = this;
    while (current && current->m_parent && current->m_parent != current->m_root_state) {
        depth++;
        current = current->m_parent;
    }
    if (current && current->m_parent == current->m_root_state) { depth++; }

    // Use a fixed-size array with a reasonable maximum depth (compliant with
    // MSVC)
    constexpr int k_max_depth = 32;  // Compile-time constant
    std::array<std::string_view, k_max_depth> components;

    if (depth > k_max_depth) {
        // Fallback for extremely deep hierarchies - just return the name
        return m_name;
    }

    // Fill the components array from leaf to root
    current = this;
    int index = depth - 1;
    while (current && current->m_parent && current->m_parent != current->m_root_state &&
           index >= 0) {
        components[index] = current->m_name;
        current = current->m_parent;
        index--;
    }
    if (current && current->m_parent == current->m_root_state && index >= 0) {
        components[index] = current->m_name;
    }

    // Build the path from root to leaf
    for (int i = 0; i < depth; i++) {
        if (i > 0) { m_root_state->m_temp_buffer_1() += '.'; }
        m_root_state->m_temp_buffer_1().append(components[i].data(), components[i].size());
    }

    return m_root_state->m_temp_buffer_1();
}

// Helper for parameter resolution with paths
std::pair<StateGroup*, std::string_view> StateGroup::resolve_path(std::string_view path) const {
    // If path is empty or has no dots, it refers to a parameter in this group
    if (path.empty() || path.find('.') == std::string::npos) {
        return {const_cast<StateGroup*>(this), path};
    }

    // Split the path into first component and the rest
    auto [group_name, rest] = detail::split_path(path);

    // Look for the group using RCU
    StateGroup* found_group = nullptr;
    m_groups_rcu.read([&](const GroupMap& groups) {
        auto group_it = groups.find(std::string(group_name));
        if (group_it != groups.end()) { found_group = group_it->second.get(); }
    });

    if (found_group) { return found_group->resolve_path(rest); }

    // If not found, throw exception
    throw StateGroupNotFoundException(group_name);
}

// Helper for parameter resolution with paths that creates missing groups
std::pair<StateGroup*, std::string_view> StateGroup::resolve_path_create(std::string_view path) {
    // If path is empty or has no dots, it refers to a parameter in this group
    if (path.empty() || path.find('.') == std::string::npos) { return {this, path}; }

    // Split the path into first component and the rest
    auto [group_name, rest] = detail::split_path(path);

    // Look for the group or create it if it doesn't exist using RCU
    StateGroup* child_group = nullptr;
    m_groups_rcu.read([&](const GroupMap& groups) {
        auto group_it = groups.find(std::string(group_name));
        if (group_it != groups.end()) { child_group = group_it->second.get(); }
    });

    if (!child_group) {
        // Create the missing group
        child_group = create_group(group_name);
    }

    // Continue resolving the rest of the path, creating groups as needed
    return child_group->resolve_path_create(rest);
}

// Parameter access methods
template <typename T>
T StateGroup::get(std::string_view path, bool allow_blocking) const TANH_NONBLOCKING_FUNCTION {
    // String access requires allow_blocking=true as it may allocate memory
    if constexpr (std::is_same_v<T, std::string>) {
        if (!allow_blocking) { throw BlockingException(path); }
    }

    // Ensure thread is registered for RCU and buffers
    m_root_state->ensure_thread_registered();

    auto getter_fn = [&]() -> T {
        auto [group, param_name] = resolve_path(path);
        if (group == this) {
            // Parameter in this group, delegate to root state
            // Use State's pre-allocated buffer
            m_root_state->m_temp_buffer_2().clear();

            std::string_view group_path = get_full_path();
            detail::join_path(group_path, param_name, m_root_state->m_temp_buffer_2());
            return m_root_state->get_from_root<T>(m_root_state->m_temp_buffer_2(), allow_blocking);
        }
        // Parameter in a child group
        return group->get<T>(param_name, allow_blocking);
    };

    if (!allow_blocking) {  // NOLINT(bugprone-branch-clone)
        return getter_fn();
    } else {
        TANH_NONBLOCKING_SCOPED_DISABLER
        return getter_fn();
    }
}

ParameterType StateGroup::get_parameter_type(std::string_view path) const
    TANH_NONBLOCKING_FUNCTION {
    // Ensure thread is registered for RCU and buffers
    m_root_state->ensure_thread_registered();

    auto [group, param_name] = resolve_path(path);
    if (group == this) {
        // Parameter in this group, delegate to root state
        // Use State's pre-allocated buffer
        m_root_state->m_temp_buffer_3().clear();

        std::string_view group_path = get_full_path();
        detail::join_path(group_path, param_name, m_root_state->m_temp_buffer_3());
        return m_root_state->get_type_from_root(m_root_state->m_temp_buffer_3());
    }
    // Parameter in a child group
    return group->get_parameter_type(param_name);
}

Parameter StateGroup::get_parameter(std::string_view path) const {
    auto [group, param_name] = resolve_path(path);
    if (group == this) {
        // Parameter in this group
        return {this, param_name, m_root_state};
    }
    // Parameter in a child group
    return group->get_parameter(param_name);
}

// Get all parameters in this group
std::map<std::string, Parameter> StateGroup::get_parameters() const {
    std::map<std::string, Parameter> params = {};
    std::string_view full_path = get_full_path();

    // Use RCU index to read parameters (lock-free)
    m_root_state->m_index_rcu.read([&](const auto& idx) {
        // Special case for root StateGroup (empty path)
        if (full_path.empty()) {
            for (const auto& [key, record] : idx) {
                params.emplace(key, Parameter(m_root_state, key));
            }
            return;
        }

        // For non-root groups, we need to check if the parameter belongs to
        // this group or its subgroups
        for (const auto& [key, record] : idx) {
            // Check if key starts with the full path and is either:
            // 1. Exactly equal to the full path (shouldn't happen with proper
            // hierarchical paths)
            // 2. Followed by a dot (meaning it's part of this group or a
            // subgroup)
            if (key.find(full_path) == 0 &&
                (key.length() == full_path.length() || key[full_path.length()] == '.')) {
                // Parameter belongs to this group
                params.emplace(key, Parameter(m_root_state, key));
            }
        }
    });

    return params;
}

// Parameter setters with path support
template <typename T>
void StateGroup::set(std::string_view path,
                     const T& value,
                     NotifyStrategies strategy,
                     ParameterListener* source,
                     bool create) {
    // Ensure thread is registered for RCU and buffers
    m_root_state->ensure_thread_registered();

    // Use different path resolution based on whether we want to create missing
    // elements
    std::pair<StateGroup*, std::string> resolution;
    if (create) {
        // Create groups as needed while resolving the path
        resolution = resolve_path_create(path);
    } else {
        // Don't create new groups, just try to find existing ones
        resolution = resolve_path(path);
    }

    auto [group, param_name] = resolution;
    if (group == this) {
        // Parameter in this group, delegate to root state
        // Use State's pre-allocated buffer
        m_root_state->m_temp_buffer_2().clear();

        std::string_view group_path = get_full_path();
        detail::join_path(group_path, param_name, m_root_state->m_temp_buffer_2());

        // If not creating and the parameter doesn't exist, check using RCU
        if (!create) {
            bool parameter_exists = false;
            m_root_state->m_index_rcu.read([&](const auto& idx) {
                parameter_exists = idx.find(m_root_state->m_temp_buffer_2()) != idx.end();
            });

            if (!parameter_exists) {
                throw StateKeyNotFoundException(m_root_state->m_temp_buffer_2());
            }
        }

        m_root_state->set_in_root(m_root_state->m_temp_buffer_2(),
                                  value,
                                  strategy,
                                  source);  // Pass the notify parameter
    } else {
        // Parameter in a child group
        group->set(param_name, value, strategy, source, create);
    }
}

// After all the StateGroup::set implementations, add the const char* overload
void StateGroup::set(std::string_view path,
                     const char* value,
                     NotifyStrategies strategy,
                     ParameterListener* source,
                     bool create) {
    set(path, std::string(value), strategy, source, create);
}

// Set with parameter definition (registers definition and sets default value)
// Private parameter definition setter implementation
void StateGroup::set(std::string_view path,
                     const ParameterDefinition& def,
                     NotifyStrategies strategy,
                     ParameterListener* source,
                     bool create) {
    // Ensure thread is registered for RCU and buffers
    m_root_state->ensure_thread_registered();

    switch (def.m_type) {
        case PluginParamType::ParamFloat:
            set(path, def.as_float(), strategy, source, create);
            break;
        case PluginParamType::ParamInt: set(path, def.as_int(), strategy, source, create); break;
        case PluginParamType::ParamBool: set(path, def.as_bool(), strategy, source, create); break;
        case PluginParamType::ParamChoice: set(path, def.as_int(), strategy, source, create); break;
    }

    // Then store the definition (need to construct the full path)
    m_root_state->m_temp_buffer_3().clear();
    std::string_view group_path = get_full_path();
    detail::join_path(group_path, path, m_root_state->m_temp_buffer_3());
    m_root_state->set_definition_in_root(m_root_state->m_temp_buffer_3(), def);
}

void StateGroup::ensure_thread_registered() {
    if (t_registered_states().find(this) != t_registered_states().end()) [[likely]] {
        return;  // Already registered
    }
    // Register this thread with all RCU structures for this StateGroup
    m_groups_rcu.register_reader_thread();
    m_listeners_rcu.register_reader_thread();
    ensure_child_groups_registered();
    // Mark this StateGroup as registered for this thread
    t_registered_states().insert(this);
}

void StateGroup::ensure_child_groups_registered() {
    m_groups_rcu.read([](const GroupMap& groups) {
        for (const auto& [name, group] : groups) { group->ensure_thread_registered(); }
    });
}

// Template specializations for parameter definition types - forward to private
// base implementation
template <>
void StateGroup::set<ParameterFloat>(std::string_view path,
                                     const ParameterFloat& value,
                                     NotifyStrategies strategy,
                                     ParameterListener* source,
                                     bool create) {
    set(path, static_cast<const ParameterDefinition&>(value), strategy, source, create);
}

template <>
void StateGroup::set<ParameterInt>(std::string_view path,
                                   const ParameterInt& value,
                                   NotifyStrategies strategy,
                                   ParameterListener* source,
                                   bool create) {
    set(path, static_cast<const ParameterDefinition&>(value), strategy, source, create);
}

template <>
void StateGroup::set<ParameterBool>(std::string_view path,
                                    const ParameterBool& value,
                                    NotifyStrategies strategy,
                                    ParameterListener* source,
                                    bool create) {
    set(path, static_cast<const ParameterDefinition&>(value), strategy, source, create);
}

template <>
void StateGroup::set<ParameterChoice>(std::string_view path,
                                      const ParameterChoice& value,
                                      NotifyStrategies strategy,
                                      ParameterListener* source,
                                      bool create) {
    set(path, static_cast<const ParameterDefinition&>(value), strategy, source, create);
}

template void StateGroup::set(std::string_view path,
                              const double& value,
                              NotifyStrategies strategy,
                              ParameterListener* source,
                              bool create);
template void StateGroup::set(std::string_view path,
                              const float& value,
                              NotifyStrategies strategy,
                              ParameterListener* source,
                              bool create);
template void StateGroup::set(std::string_view path,
                              const int& value,
                              NotifyStrategies strategy,
                              ParameterListener* source,
                              bool create);
template void StateGroup::set(std::string_view path,
                              const bool& value,
                              NotifyStrategies strategy,
                              ParameterListener* source,
                              bool create);
template void StateGroup::set(std::string_view path,
                              const std::string& value,
                              NotifyStrategies strategy,
                              ParameterListener* source,
                              bool create);

template double StateGroup::get(std::string_view path,
                                bool allow_blocking) const TANH_NONBLOCKING_FUNCTION;
template float StateGroup::get(std::string_view path,
                               bool allow_blocking) const TANH_NONBLOCKING_FUNCTION;
template int StateGroup::get(std::string_view path,
                             bool allow_blocking) const TANH_NONBLOCKING_FUNCTION;
template bool StateGroup::get(std::string_view path,
                              bool allow_blocking) const TANH_NONBLOCKING_FUNCTION;
template std::string StateGroup::get(std::string_view path,
                                     bool allow_blocking) const TANH_NONBLOCKING_FUNCTION;
}  // namespace thl