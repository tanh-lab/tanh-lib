#include "tanh/state/StateGroup.h"
#include "tanh/state/State.h"

namespace thl {

// StateGroup state management methods
void StateGroup::clear_groups() {
    // First, collect all groups to clear (to avoid recursive RCU updates)
    std::vector<std::shared_ptr<StateGroup>> groups_to_clear;
    
    m_groups_rcu.read([&](const GroupMap& groups) {
        groups_to_clear.reserve(groups.size());
        for (const auto& [name, group] : groups) {
            groups_to_clear.push_back(group);
        }
    });
    
    // Clear all subgroups outside of RCU context
    for (auto& group : groups_to_clear) {
        group->clear();
    }
    
    // Now clear the groups map
    m_groups_rcu.update([&](GroupMap& groups) {
        groups.clear();
    });
}

void StateGroup::clear() {
    // Clear parameters in this group
    if (m_parent == nullptr) {
        // If this is the root state, clear parameters - already thread safe
        m_rootState->clear();
    } else {
        // Get the full path once to avoid repeated calls
        std::string_view fullPath = get_full_path();
        
        // Use RCU to update parameters map
        m_rootState->m_parameters_rcu.update([&](auto& parameters) {
            // First pass: collect parameters to delete
            std::vector<std::string> keys_to_delete;
            keys_to_delete.reserve(parameters.size()); // Prevent reallocation
            
            for (const auto& [key, param] : parameters) {
                // String operations like find() aren't real-time safe, but we're doing this under RCU update
                // and with keys_to_delete pre-allocated to avoid allocation during iteration
                if (key.find(fullPath) == 0) {
                    // If the parameter belongs to this group, mark it for deletion
                    keys_to_delete.push_back(key);
                }
            }
            
            // Second pass: delete the parameters
            for (const auto& key : keys_to_delete) {
                parameters.erase(key);
            }
        });
    } 
    clear_groups();
}

bool StateGroup::is_empty() const [[clang::nonblocking]] {
    // Check if we have any subgroups using RCU (lock-free read)
    bool has_groups = false;
    m_groups_rcu.read([&](const GroupMap& groups) {
        has_groups = !groups.empty();
    });
    
    if (has_groups) {
        return false;
    }

    // Get the full path once to avoid repeated calls - real-time safe
    std::string_view fullPath = get_full_path();
    
    // Use RCU to check if this group has any parameters (lock-free read)
    bool has_parameters = false;
    m_rootState->m_parameters_rcu.read([&](const auto& parameters) {
        for (const auto& [key, param] : parameters) {
            // String operations like find() aren't real-time safe, but we're doing this under RCU read
            // and in a controlled environment where we know the strings won't cause memory allocation
            if (key.find(fullPath) == 0) {
                // If the parameter belongs to this group, we're not empty
                has_parameters = true;
                return; // Early exit from lambda
            }
        }
    });
    
    return !has_parameters;
}

// StateGroup listener management
void StateGroup::add_listener(ParameterListener* listener) {
    m_listeners_rcu.update([&](ListenerData& data) {
        // Check if listener already exists
        auto it = std::find(data.object_listeners.begin(), data.object_listeners.end(), listener);
        if (it == data.object_listeners.end()) {
            data.object_listeners.push_back(listener);
        }
    });
}

void StateGroup::remove_listener(ParameterListener* listener) {
    m_listeners_rcu.update([&](ListenerData& data) {
        auto it = std::find(data.object_listeners.begin(), data.object_listeners.end(), listener);
        if (it != data.object_listeners.end()) {
            data.object_listeners.erase(it);
        }
    });
}

size_t StateGroup::add_callback_listener(ParameterChangeCallback callback) {
    size_t id = 0;
    m_listeners_rcu.update([&](ListenerData& data) {
        id = data.next_listener_id++;
        data.callback_listeners[id] = std::move(callback);
    });
    return id;
}

void StateGroup::remove_callback_listener(size_t listener_id) {
    m_listeners_rcu.update([&](ListenerData& data) {
        data.callback_listeners.erase(listener_id);
    });
}

void StateGroup::notify_parameter_change(std::string_view path) [[clang::nonblocking]] {
    // Resolve the path to get the parameter
    auto [group, param_name] = resolve_path(path);
    if (!group || !group->m_rootState) return;
    
    try {
        // Get the parameter
        // Use State's pre-allocated buffer
        m_rootState->m_path_buffer_1.clear();
        
        std::string_view group_path = group->get_full_path();
        detail::join_path(group_path, param_name, m_rootState->m_path_buffer_1);
        Parameter param = group->m_rootState->get_from_root(m_rootState->m_path_buffer_1);
        
        // Notify all listeners
        notify_listeners(path, param);
    } catch (const StateKeyNotFoundException&) {
        // Parameter not found, do nothing
    }
}

void StateGroup::notify_listeners(std::string_view path, const Parameter& param) const [[clang::nonblocking]] {
    // Notify listeners at this level using RCU (lock-free read)
    m_listeners_rcu.read([&](const ListenerData& data) {
        // Notify object-based listeners
        for (auto listener : data.object_listeners) {
            listener->on_parameter_changed(path, param);
        }
        
        // Notify callback-based listeners
        for (const auto& [id, callback] : data.callback_listeners) {
            callback(path, param);
        }
    });
    
    // Then, notify parent groups to propagate up the hierarchy
    if (m_parent) {
        m_parent->notify_listeners(path, param);
    }
}

// StateGroup implementation
StateGroup::StateGroup(State* rootState, StateGroup* parent, std::string_view name)
    : m_rootState(rootState), m_parent(parent), m_name(name) {
}

// Group management
StateGroup* StateGroup::create_group(std::string_view name) {
    std::string name_str(name);
    
    // First, check if group already exists
    StateGroup* existing_group = nullptr;
    m_groups_rcu.read([&](const GroupMap& groups) {
        auto it = groups.find(name_str);
        if (it != groups.end()) {
            existing_group = it->second.get();
        }
    });
    
    if (existing_group) {
        return existing_group;
    }
    
    // Create new group and add it to the map
    auto new_group = std::make_shared<StateGroup>(m_rootState, this, name);
    auto* group_ptr = new_group.get();
    
    m_groups_rcu.update([&](GroupMap& groups) {
        groups[name_str] = new_group;
    });
    
    return group_ptr;
}

StateGroup* StateGroup::get_group(std::string_view name) const [[clang::nonblocking]] {
    std::string name_str(name);
    StateGroup* found_group = nullptr;
    
    m_groups_rcu.read([&](const GroupMap& groups) {
        auto it = groups.find(name_str);
        if (it != groups.end()) {
            found_group = it->second.get();
        }
    });
    
    return found_group;
}

bool StateGroup::has_group(std::string_view name) const [[clang::nonblocking]] {
    std::string name_str(name);
    bool found = false;
    
    m_groups_rcu.read([&](const GroupMap& groups) {
        found = groups.find(name_str) != groups.end();
    });
    
    return found;
}

// Get the full path of this group
std::string_view StateGroup::get_full_path() const [[clang::nonblocking]] {
    if (!m_parent || m_parent == m_rootState) {
        return m_name;
    }
    
    // Use State's pre-allocated buffer
    m_rootState->m_path_buffer_1.clear();
    
    // Build path iteratively by calculating depth first, then building from root
    // Count the depth to avoid recursion
    int depth = 0;
    const StateGroup* current = this;
    while (current && current->m_parent && current->m_parent != current->m_rootState) {
        depth++;
        current = current->m_parent;
    }
    if (current && current->m_parent == current->m_rootState) {
        depth++;
    }
    
    // Now build the path by walking up the tree and indexing backwards
    // We'll use a fixed-size array on the stack for path components
    std::string_view components[m_rootState->m_max_levels];

    if (depth > m_rootState->m_max_levels) {
        // Fallback for extremely deep hierarchies - just return the name
        return m_name;
    }
    
    // Fill the components array from leaf to root
    current = this;
    int index = depth - 1;
    while (current && current->m_parent && current->m_parent != current->m_rootState && index >= 0) {
        components[index] = current->m_name;
        current = current->m_parent;
        index--;
    }
    if (current && current->m_parent == current->m_rootState && index >= 0) {
        components[index] = current->m_name;
    }
    
    // Build the path from root to leaf
    for (int i = 0; i < depth; i++) {
        if (i > 0) {
            m_rootState->m_path_buffer_1 += '.';
        }
        m_rootState->m_path_buffer_1.append(components[i].data(), components[i].size());
    }
    
    return m_rootState->m_path_buffer_1;
}

// Helper for parameter resolution with paths
std::pair<StateGroup*, std::string_view> StateGroup::resolve_path(std::string_view path) const [[clang::nonblocking]] {
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
        if (group_it != groups.end()) {
            found_group = group_it->second.get();
        }
    });
    
    if (found_group) {
        return found_group->resolve_path(rest);
    }

    // If group not found, return this group and an empty string
    return {const_cast<StateGroup*>(this), std::string("")};
}

// Helper for parameter resolution with paths that creates missing groups
std::pair<StateGroup*, std::string_view> StateGroup::resolve_path_create(std::string_view path) {
    // If path is empty or has no dots, it refers to a parameter in this group
    if (path.empty() || path.find('.') == std::string::npos) {
        return {this, path};
    }
    
    // Split the path into first component and the rest
    auto [group_name, rest] = detail::split_path(path);
    
    // Look for the group or create it if it doesn't exist using RCU
    StateGroup* child_group = nullptr;
    m_groups_rcu.read([&](const GroupMap& groups) {
        auto group_it = groups.find(std::string(group_name));
        if (group_it != groups.end()) {
            child_group = group_it->second.get();
        }
    });
    
    if (!child_group) {
        // Create the missing group
        child_group = create_group(group_name);
    }
    
    // Continue resolving the rest of the path, creating groups as needed
    return child_group->resolve_path_create(rest);
}

// Parameter access methods
template<typename T>
T StateGroup::get(std::string_view path) const [[clang::nonblocking]] {
    auto [group, param_name] = resolve_path(path);
    if (group == this) {
        // Parameter in this group, delegate to root state
        // Use State's pre-allocated buffer
        m_rootState->m_path_buffer_2.clear();
        
        std::string_view group_path = get_full_path();
        detail::join_path(group_path, param_name, m_rootState->m_path_buffer_2);
        return m_rootState->get_from_root<T>(m_rootState->m_path_buffer_2);
    }
    // Parameter in a child group
    return group->get<T>(param_name);
}

ParameterType StateGroup::get_parameter_type(std::string_view path) const [[clang::nonblocking]] {
    auto [group, param_name] = resolve_path(path);
    if (group == this) {
        // Parameter in this group, delegate to root state
        // Use State's pre-allocated buffer
        m_rootState->m_path_buffer_3.clear();
        
        std::string_view group_path = get_full_path();
        detail::join_path(group_path, param_name, m_rootState->m_path_buffer_3);
        return m_rootState->get_type_from_root(m_rootState->m_path_buffer_3);
    }
    // Parameter in a child group
    return group->get_parameter_type(param_name);
}

Parameter StateGroup::get_parameter(std::string_view path) const {
    auto [group, param_name] = resolve_path(path);
    if (group == this) {
        // Parameter in this group
        return Parameter(this, param_name, m_rootState);
    }
    // Parameter in a child group
    return group->get_parameter(param_name);
}

// Get all parameters in this group
std::map<std::string, Parameter> StateGroup::get_parameters() const {
    std::map<std::string, Parameter> params = {};
    std::string_view fullPath = get_full_path();
    
    // Use RCU to read parameters (lock-free)
    m_rootState->m_parameters_rcu.read([&](const auto& parameters) {
        // Special case for root StateGroup (empty path)
        if (fullPath.empty()) {
            for (const auto& [key, param] : parameters) {
                params.emplace(key, Parameter(m_rootState, key));
            }
            return;
        }
        
        // For non-root groups, we need to check if the parameter belongs to this group or its subgroups
        for (const auto& [key, param] : parameters) {
            // Check if key starts with the full path and is either:
            // 1. Exactly equal to the full path (shouldn't happen with proper hierarchical paths)
            // 2. Followed by a dot (meaning it's part of this group or a subgroup)
            if (key.find(fullPath) == 0 && 
                (key.length() == fullPath.length() || key[fullPath.length()] == '.')) {
                // Parameter belongs to this group
                params.emplace(key, Parameter(m_rootState, key));
            }
        }
    });
    
    return params;
}

// Parameter setters with path support
template<typename T>
void StateGroup::set(std::string_view path, T value, bool notify, bool create) {

    // Use different path resolution based on whether we want to create missing elements
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
        m_rootState->m_path_buffer_2.clear();
        
        std::string_view group_path = get_full_path();
        detail::join_path(group_path, param_name, m_rootState->m_path_buffer_2);
        
        // If not creating and the parameter doesn't exist, check using RCU
        if (!create) {
            bool parameter_exists = false;
            m_rootState->m_parameters_rcu.read([&](const auto& parameters) {
                parameter_exists = parameters.find(m_rootState->m_path_buffer_2) != parameters.end();
            });
            
            if (!parameter_exists) {
                throw StateKeyNotFoundException(m_rootState->m_path_buffer_2);
            }
        }
        
        m_rootState->set_in_root(m_rootState->m_path_buffer_2, value, notify); // Pass the notify parameter
    } else {
        // Parameter in a child group
        group->set(param_name, value, notify, create);
    }
}

// After all the StateGroup::set implementations, add the const char* overload
void StateGroup::set(std::string_view path, const char* value, bool notify, bool create) {
    set(path, std::string(value), notify, create);
}

template void StateGroup::set(std::string_view path, const double value, bool notify, bool create);
template void StateGroup::set(std::string_view path, const float value, bool notify, bool create);
template void StateGroup::set(std::string_view path, const int value, bool notify, bool create);
template void StateGroup::set(std::string_view path, const bool value, bool notify, bool create);
template void StateGroup::set(std::string_view path, const std::string value, bool notify, bool create);


template double StateGroup::get(std::string_view path) const [[clang::nonblocking]];
template float StateGroup::get(std::string_view path) const [[clang::nonblocking]];
template int StateGroup::get(std::string_view path) const [[clang::nonblocking]];
template bool StateGroup::get(std::string_view path) const [[clang::nonblocking]];
template std::string StateGroup::get(std::string_view path) const [[clang::nonblocking]];

} // namespace thl