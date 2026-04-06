#include "tanh/state/StateGroup.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tanh/core/Exports.h"
#include "tanh/state/Parameter.h"
#include "tanh/state/ParameterDefinitions.h"
#include "tanh/state/State.h"
#include "tanh/state/path_helpers.h"
#include "tanh/utils/RealtimeSanitizer.h"
#include "tanh/state/Exceptions.h"
#include "tanh/state/ParameterListener.h"

namespace thl {

// NOLINTBEGIN(misc-no-recursion)

// ── Constructor / Destructor ────────────────────────────────────────────────

StateGroup::StateGroup(State* root_state, StateGroup* parent, std::string_view name)
    : m_root_state(root_state), m_parent(parent), m_name(name) {}

StateGroup::~StateGroup() {
    t_registered_states().erase(this);
}

// ── Group management ────────────────────────────────────────────────────────

StateGroup* StateGroup::create_group(std::string_view name) {
    std::string name_str(name);

    StateGroup* existing_group = nullptr;
    m_groups_rcu.read([&](const GroupMap& groups) {
        auto it = groups.find(name_str);
        if (it != groups.end()) { existing_group = it->second.get(); }
    });

    if (existing_group) { return existing_group; }

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

// ── Path utilities ──────────────────────────────────────────────────────────

std::string_view StateGroup::get_full_path() const TANH_NONBLOCKING_FUNCTION {
    if (!m_parent || m_parent == m_root_state) { return m_name; }

    m_root_state->ensure_thread_registered();

    m_root_state->m_temp_buffer_1().clear();

    int depth = 0;
    const StateGroup* current = this;
    while (current && current->m_parent && current->m_parent != current->m_root_state) {
        depth++;
        current = current->m_parent;
    }
    if (current && current->m_parent == current->m_root_state) { depth++; }

    constexpr int k_max_depth = 32;
    std::array<std::string_view, k_max_depth> components;

    if (depth > k_max_depth) { return m_name; }

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

    for (int i = 0; i < depth; i++) {
        if (i > 0) { m_root_state->m_temp_buffer_1() += '.'; }
        m_root_state->m_temp_buffer_1().append(components[i].data(), components[i].size());
    }

    return m_root_state->m_temp_buffer_1();
}

std::pair<StateGroup*, std::string_view> StateGroup::resolve_path(std::string_view path) const {
    if (path.empty() || path.find('.') == std::string::npos) {
        return {const_cast<StateGroup*>(this), path};
    }

    auto [group_name, rest] = detail::split_path(path);

    StateGroup* found_group = nullptr;
    m_groups_rcu.read([&](const GroupMap& groups) {
        auto group_it = groups.find(std::string(group_name));
        if (group_it != groups.end()) { found_group = group_it->second.get(); }
    });

    if (found_group) { return found_group->resolve_path(rest); }

    throw StateGroupNotFoundException(group_name);
}

std::pair<StateGroup*, std::string_view> StateGroup::resolve_path_create(std::string_view path) {
    if (path.empty() || path.find('.') == std::string::npos) { return {this, path}; }

    auto [group_name, rest] = detail::split_path(path);

    StateGroup* child_group = nullptr;
    m_groups_rcu.read([&](const GroupMap& groups) {
        auto group_it = groups.find(std::string(group_name));
        if (group_it != groups.end()) { child_group = group_it->second.get(); }
    });

    if (!child_group) { child_group = create_group(group_name); }

    return child_group->resolve_path_create(rest);
}

// ── Parameter creation ──────────────────────────────────────────────────────

void StateGroup::create(std::string_view path, ParameterDefinition def) {
    m_root_state->ensure_thread_registered();

    auto [group, param_name] = resolve_path_create(path);

    // Build full key
    m_root_state->m_temp_buffer_2().clear();
    std::string_view const group_path = group->get_full_path();
    detail::join_path(group_path, param_name, m_root_state->m_temp_buffer_2());

    m_root_state->create_in_root(m_root_state->m_temp_buffer_2(), std::move(def));
}

template <typename T>
void StateGroup::create(std::string_view path, const T& value) {
    m_root_state->ensure_thread_registered();

    auto [group, param_name] = resolve_path_create(path);

    m_root_state->m_temp_buffer_2().clear();
    std::string_view const group_path = group->get_full_path();
    detail::join_path(group_path, param_name, m_root_state->m_temp_buffer_2());

    m_root_state->create_in_root(m_root_state->m_temp_buffer_2(), value);
}

void StateGroup::create(std::string_view path, const char* value) {
    create(path, std::string(value));
}

// ── Parameter update ────────────────────────────────────────────────────────

template <typename T>
void StateGroup::set(std::string_view path, const T& value, ParameterListener* source) {
    m_root_state->ensure_thread_registered();

    auto [group, param_name] = resolve_path(path);
    if (group == this) {
        m_root_state->m_temp_buffer_2().clear();
        std::string_view const group_path = get_full_path();
        detail::join_path(group_path, param_name, m_root_state->m_temp_buffer_2());

        m_root_state->set_in_root(m_root_state->m_temp_buffer_2(), value, source);
    } else {
        group->set(param_name, value, source);
    }
}

void StateGroup::set(std::string_view path, const char* value, ParameterListener* source) {
    set(path, std::string(value), source);
}

// ── Parameter access ────────────────────────────────────────────────────────

template <typename T>
T StateGroup::get(std::string_view path, bool allow_blocking) const TANH_NONBLOCKING_FUNCTION {
    if constexpr (std::is_same_v<T, std::string>) {
        if (!allow_blocking) { throw BlockingException(path); }
    }

    m_root_state->ensure_thread_registered();

    auto getter_fn = [&]() -> T {
        auto [group, param_name] = resolve_path(path);
        if (group == this) {
            m_root_state->m_temp_buffer_2().clear();
            std::string_view const group_path = get_full_path();
            detail::join_path(group_path, param_name, m_root_state->m_temp_buffer_2());
            return m_root_state->get_from_root<T>(m_root_state->m_temp_buffer_2(), allow_blocking);
        }
        return group->get<T>(param_name, allow_blocking);
    };

    if (!allow_blocking) {  // NOLINT(bugprone-branch-clone)
        return getter_fn();
    }
    TANH_NONBLOCKING_SCOPED_DISABLER
    return getter_fn();
}

Parameter StateGroup::get_parameter(std::string_view path) const {
    auto [group, param_name] = resolve_path(path);
    if (group == this) {
        m_root_state->m_temp_buffer_2().clear();
        std::string_view const group_path = get_full_path();
        detail::join_path(group_path, param_name, m_root_state->m_temp_buffer_2());
        return m_root_state->get_parameter_from_root(m_root_state->m_temp_buffer_2());
    }
    return group->get_parameter(param_name);
}

std::map<std::string, Parameter> StateGroup::get_parameters() const {
    std::map<std::string, Parameter> params = {};
    std::string_view full_path = get_full_path();

    m_root_state->m_string_index_rcu.read([&](const auto& idx) {
        if (full_path.empty()) {
            for (const auto& [key, record] : idx) {
                params.emplace(key, Parameter(m_root_state, record));
            }
            return;
        }

        for (const auto& [key, record] : idx) {
            if (key.find(full_path) == 0 &&
                (key.length() == full_path.length() || key[full_path.length()] == '.')) {
                params.emplace(key, Parameter(m_root_state, record));
            }
        }
    });

    return params;
}

ParameterType StateGroup::get_parameter_type(std::string_view path) const
    TANH_NONBLOCKING_FUNCTION {
    m_root_state->ensure_thread_registered();

    auto [group, param_name] = resolve_path(path);
    if (group == this) {
        m_root_state->m_temp_buffer_3().clear();
        std::string_view const group_path = get_full_path();
        detail::join_path(group_path, param_name, m_root_state->m_temp_buffer_3());
        return m_root_state->get_type_from_root(m_root_state->m_temp_buffer_3());
    }
    return group->get_parameter_type(param_name);
}

template <typename T>
ParameterHandle<T> StateGroup::get_handle(std::string_view path) const {
    auto [group, param_name] = resolve_path(path);
    if (group == this) {
        m_root_state->m_temp_buffer_2().clear();
        std::string_view const group_path = get_full_path();
        detail::join_path(group_path, param_name, m_root_state->m_temp_buffer_2());
        return m_root_state->get_handle_from_root<T>(m_root_state->m_temp_buffer_2());
    }
    return group->get_handle<T>(param_name);
}

// ── Gesture ─────────────────────────────────────────────────────────────────

void StateGroup::set_gesture(std::string_view path, bool gesture) {
    auto [group, param_name] = resolve_path(path);
    if (group == this) {
        m_root_state->m_temp_buffer_2().clear();
        std::string_view const group_path = get_full_path();
        detail::join_path(group_path, param_name, m_root_state->m_temp_buffer_2());
        m_root_state->set_gesture_from_root(m_root_state->m_temp_buffer_2(), gesture);
        return;
    }
    group->set_gesture(param_name, gesture);
}

// ── Listener management ─────────────────────────────────────────────────────

void StateGroup::add_listener(ParameterListener* listener) {
    m_listeners_rcu.update([&](ListenerData& data) {
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

void StateGroup::notify_parameter_change(std::string_view path) {
    auto [group, param_name] = resolve_path(path);
    if (!group || !group->m_root_state) { return; }

    m_root_state->ensure_thread_registered();

    try {
        m_root_state->m_temp_buffer_1().clear();

        std::string_view const group_path = group->get_full_path();
        detail::join_path(group_path, param_name, m_root_state->m_temp_buffer_1());
        Parameter const param =
            group->m_root_state->get_parameter_from_root(m_root_state->m_temp_buffer_1());

        notify_listeners(param);
    } catch (const StateKeyNotFoundException&) {
        // Parameter may not exist — silently ignore
        return;
    }
}

void StateGroup::notify_listeners(const Parameter& param, ParameterListener* source) const {
    // Determine strategy from source listener (default: notify all)
    const NotifyStrategies strategy = source ? source->m_strategy : NotifyStrategies::All;

    if (strategy == NotifyStrategies::None) { return; }

    // Read gesture state from the record
    const bool in_gesture = param.m_record->m_in_gesture.load(std::memory_order_relaxed);

    m_listeners_rcu.read([&](const ListenerData& data) {
        for (auto* listener : data.m_object_listeners) {
            if ((strategy == NotifyStrategies::Others && listener == source) ||
                (strategy == NotifyStrategies::Self && listener != source)) {
                continue;
            }
            if (in_gesture && !listener->m_receives_during_gesture) { continue; }
            listener->on_parameter_changed(param);
        }
    });

    if (m_parent) { m_parent->notify_listeners(param, source); }
}

// ── State management ────────────────────────────────────────────────────────

void StateGroup::clear_groups() {
    std::vector<std::shared_ptr<StateGroup>> groups_to_clear;

    m_groups_rcu.read([&](const GroupMap& groups) {
        groups_to_clear.reserve(groups.size());
        for (const auto& [name, group] : groups) { groups_to_clear.push_back(group); }
    });

    for (auto& group : groups_to_clear) { group->clear(); }

    m_groups_rcu.update([&](GroupMap& groups) { groups.clear(); });
}

void StateGroup::clear() {
    if (m_parent == nullptr) {
        m_root_state->clear();
    } else {
        std::string_view const full_path = get_full_path();

        std::vector<std::string> keys_to_delete;
        {
            std::scoped_lock const lock(m_root_state->m_storage_mutex);
            for (const auto& [key, record] : m_root_state->m_storage) {
                if (key.starts_with(full_path)) { keys_to_delete.push_back(key); }
            }
        }

        m_root_state->m_string_index_rcu.update([&](auto& idx) {
            for (const auto& key : keys_to_delete) { idx.erase(key); }
        });

        // Remove deleted parameters from the ID index
        m_root_state->m_id_index_rcu.update([&](auto& idx) {
            std::erase_if(idx, [&](const auto& pair) {
                for (const auto& key : keys_to_delete) {
                    if (pair.second->m_key == key) { return true; }
                }
                return false;
            });
        });

        {
            std::scoped_lock const lock(m_root_state->m_storage_mutex);
            for (const auto& key : keys_to_delete) { m_root_state->m_storage.erase(key); }
        }
    }
    clear_groups();
}

bool StateGroup::is_empty() const TANH_NONBLOCKING_FUNCTION {
    bool has_groups = false;
    m_groups_rcu.read([&](const GroupMap& groups) { has_groups = !groups.empty(); });

    if (has_groups) { return false; }

    std::string_view full_path = get_full_path();

    bool has_parameters = false;
    m_root_state->m_string_index_rcu.read([&](const auto& idx) {
        for (const auto& [key, record] : idx) {
            if (key.find(full_path) == 0) {
                has_parameters = true;
                return;
            }
        }
    });

    return !has_parameters;
}

// ── Thread registration ─────────────────────────────────────────────────────

void StateGroup::ensure_thread_registered() {
    if (t_registered_states().find(this) != t_registered_states().end()) [[likely]] { return; }
    m_groups_rcu.register_reader_thread();
    m_listeners_rcu.register_reader_thread();
    ensure_child_groups_registered();
    t_registered_states().insert(this);
}

void StateGroup::ensure_child_groups_registered() {
    m_groups_rcu.read([](const GroupMap& groups) {
        for (const auto& [name, group] : groups) { group->ensure_thread_registered(); }
    });
}

// ── Template specializations for ParameterDefinition subclasses ─────────────

template <>
TANH_API void StateGroup::create<ParameterFloat>(std::string_view path,
                                                 const ParameterFloat& value) {
    create(path, static_cast<const ParameterDefinition&>(value));
}

template <>
TANH_API void StateGroup::create<ParameterInt>(std::string_view path, const ParameterInt& value) {
    create(path, static_cast<const ParameterDefinition&>(value));
}

template <>
TANH_API void StateGroup::create<ParameterBool>(std::string_view path, const ParameterBool& value) {
    create(path, static_cast<const ParameterDefinition&>(value));
}

template <>
TANH_API void StateGroup::create<ParameterChoice>(std::string_view path,
                                                  const ParameterChoice& value) {
    create(path, static_cast<const ParameterDefinition&>(value));
}

// ── Template instantiations ─────────────────────────────────────────────────

template TANH_API void StateGroup::create(std::string_view path, const double& value);
template TANH_API void StateGroup::create(std::string_view path, const float& value);
template TANH_API void StateGroup::create(std::string_view path, const int& value);
template TANH_API void StateGroup::create(std::string_view path, const bool& value);
template TANH_API void StateGroup::create(std::string_view path, const std::string& value);

template TANH_API void StateGroup::set(std::string_view path,
                                       const double& value,
                                       ParameterListener* source);
template TANH_API void StateGroup::set(std::string_view path,
                                       const float& value,
                                       ParameterListener* source);
template TANH_API void StateGroup::set(std::string_view path,
                                       const int& value,
                                       ParameterListener* source);
template TANH_API void StateGroup::set(std::string_view path,
                                       const bool& value,
                                       ParameterListener* source);
template TANH_API void StateGroup::set(std::string_view path,
                                       const std::string& value,
                                       ParameterListener* source);

template TANH_API double StateGroup::get(std::string_view path,
                                         bool allow_blocking) const TANH_NONBLOCKING_FUNCTION;
template TANH_API float StateGroup::get(std::string_view path,
                                        bool allow_blocking) const TANH_NONBLOCKING_FUNCTION;
template TANH_API int StateGroup::get(std::string_view path,
                                      bool allow_blocking) const TANH_NONBLOCKING_FUNCTION;
template TANH_API bool StateGroup::get(std::string_view path,
                                       bool allow_blocking) const TANH_NONBLOCKING_FUNCTION;
template TANH_API std::string StateGroup::get(std::string_view path,
                                              bool allow_blocking) const TANH_NONBLOCKING_FUNCTION;

template TANH_API ParameterHandle<double> StateGroup::get_handle(std::string_view path) const;
template TANH_API ParameterHandle<float> StateGroup::get_handle(std::string_view path) const;
template TANH_API ParameterHandle<int> StateGroup::get_handle(std::string_view path) const;
template TANH_API ParameterHandle<bool> StateGroup::get_handle(std::string_view path) const;
// NOLINTEND(misc-no-recursion)

}  // namespace thl
