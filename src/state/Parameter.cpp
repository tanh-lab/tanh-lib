#include <tanh/state/Parameter.h>
#include <tanh/state/State.h>

namespace thl {

// Parameter class implementation
Parameter::Parameter(const State* state, std::string_view key)
    : m_state(state), m_key(key) {
}

Parameter::Parameter(const StateGroup* group, std::string_view key, const State* rootState)
    : m_state(rootState) {
    std::string_view group_path = group->get_full_path();
    size_t required_size = detail::join_path_size(group_path, key);
    m_key.reserve(required_size);
    detail::join_path(group_path, key, m_key);
}

// Parameter conversion methods
template<typename T>
T Parameter::to() const {
    if constexpr (std::is_same_v<T, double>) {
        return m_state->get_from_root<double>(m_key);
    } else if constexpr (std::is_same_v<T, float>) {
        return m_state->get_from_root<float>(m_key);
    } else if constexpr (std::is_same_v<T, int>) {
        return m_state->get_from_root<int>(m_key);
    } else if constexpr (std::is_same_v<T, bool>) {
        return m_state->get_from_root<bool>(m_key);
    } else if constexpr (std::is_same_v<T, std::string>) {
        return m_state->get_from_root<std::string>(m_key);
    } else {
        static_assert(std::is_same_v<T, void>, "Unsupported type for Parameter::to()");
        // This line will never be reached, but needed to avoid compiler warnings
        return T{};
    }
}

// Parameter type checking
ParameterType Parameter::get_type() const {
    return m_state->get_type_from_root(m_key);
}

bool Parameter::is_double() const {
    return get_type() == ParameterType::Double;
}

bool Parameter::is_float() const {
    return get_type() == ParameterType::Float;
}

bool Parameter::is_int() const {
    return get_type() == ParameterType::Int;
}

bool Parameter::is_bool() const {
    return get_type() == ParameterType::Bool;
}

bool Parameter::is_string() const {
    return get_type() == ParameterType::String;
}

// Object-oriented parameter access - renamed to get_from_root
Parameter State::get_from_root(std::string_view key) const {
    return Parameter(this, key);
}

// Parameter notification method
void Parameter::notify(NotifyStrategies strategy, ParameterListener* source) const {
    // Create a temporary parameter object for notification
    Parameter param_obj(m_state, m_key);
    
    // Check if this parameter belongs to a group by looking for dots in the path
    std::string path = m_key;
    std::string root_segment = path;
    size_t first_dot = path.find('.');
    if (first_dot != std::string::npos) {
        root_segment = path.substr(0, first_dot);
        
        // If this parameter belongs to a StateGroup, notify through that group using RCU
        auto* state = const_cast<State*>(m_state);
        StateGroup* found_group = nullptr;
        
        state->m_groups_rcu.read([&](const auto& groups) {
            auto group_it = groups.find(root_segment);
            if (group_it != groups.end()) {
                found_group = group_it->second.get();
            }
        });
        
        if (found_group) {
            // This will notify both the group listener and propagate up to the root
            found_group->notify_listeners(path, param_obj, strategy, source);
            return;
        }
    }
    
    // Otherwise notify directly from State
    const_cast<State*>(m_state)->notify_listeners(path, param_obj, strategy, source);
}

// Get full path for this parameter
std::string Parameter::get_path() const {
    return m_key;
}

template double Parameter::to<double>() const;
template float Parameter::to<float>() const;
template int Parameter::to<int>() const;
template bool Parameter::to<bool>() const;
template std::string Parameter::to<std::string>() const;

} // namespace thl