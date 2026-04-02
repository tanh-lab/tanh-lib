#include <tanh/state/Parameter.h>
#include <tanh/state/State.h>
#include <tanh/utils/RealtimeSanitizer.h>

namespace thl {

// Parameter class implementation
Parameter::Parameter(const State* state, std::string_view key) : m_state(state), m_key(key) {
    state->m_index_rcu.read([&](const auto& idx) {
        auto it = idx.find(key);
        if (it != idx.end()) {
            m_cache_ptr = &it->second->m_cache;
            m_type = it->second->m_type;
        }
    });
}

Parameter::Parameter(const StateGroup* group, std::string_view key, const State* root_state)
    : m_state(root_state) {
    std::string_view group_path = group->get_full_path();
    size_t required_size = detail::join_path_size(group_path, key);
    m_key.reserve(required_size);
    detail::join_path(group_path, key, m_key);

    root_state->m_index_rcu.read([&](const auto& idx) {
        auto it = idx.find(m_key);
        if (it != idx.end()) {
            m_cache_ptr = &it->second->m_cache;
            m_type = it->second->m_type;
        }
    });
}

// Parameter conversion methods
template <typename T>
T Parameter::to(bool allow_blocking) const TANH_NONBLOCKING_FUNCTION {
    if (!m_cache_ptr) { throw StateKeyNotFoundException(m_key); }

    if constexpr (std::is_same_v<T, std::string>) {
        if (!allow_blocking) { throw BlockingException(m_key); }
        TANH_NONBLOCKING_SCOPED_DISABLER
        switch (m_type) {
            case ParameterType::String: {
                // Must read string_value from record under storage mutex
                std::lock_guard<std::mutex> lock(m_state->m_storage_mutex);
                ParameterRecord* record = nullptr;
                m_state->m_index_rcu.read([&](const auto& idx) {
                    auto it = idx.find(m_key);
                    if (it != idx.end()) { record = it->second; }
                });
                return record ? record->m_string_value : "";
            }
            case ParameterType::Double:
                return std::to_string(m_cache_ptr->m_atomic_double.load(std::memory_order_relaxed));
            case ParameterType::Float:
                return std::to_string(m_cache_ptr->m_atomic_float.load(std::memory_order_relaxed));
            case ParameterType::Int:
                return std::to_string(m_cache_ptr->m_atomic_int.load(std::memory_order_relaxed));
            case ParameterType::Bool:
                return m_cache_ptr->m_atomic_bool.load(std::memory_order_relaxed) ? "true" : "false";
            default: return "";
        }
    } else {
        // Numeric types: load from native atomic and static_cast — no RCU needed
        switch (m_type) {
            case ParameterType::Double:
                return static_cast<T>(m_cache_ptr->m_atomic_double.load(std::memory_order_relaxed));
            case ParameterType::Float:
                return static_cast<T>(m_cache_ptr->m_atomic_float.load(std::memory_order_relaxed));
            case ParameterType::Int:
                return static_cast<T>(m_cache_ptr->m_atomic_int.load(std::memory_order_relaxed));
            case ParameterType::Bool:
                return static_cast<T>(m_cache_ptr->m_atomic_bool.load(std::memory_order_relaxed));
            case ParameterType::String:
                return static_cast<T>(m_cache_ptr->m_atomic_double.load(std::memory_order_relaxed));
            default: return T{};
        }
    }
}

// Parameter type checking — uses cached m_type, no RCU read needed
ParameterType Parameter::get_type() const TANH_NONBLOCKING_FUNCTION {
    return m_type;
}

bool Parameter::is_double() const TANH_NONBLOCKING_FUNCTION {
    return m_type == ParameterType::Double;
}

bool Parameter::is_float() const TANH_NONBLOCKING_FUNCTION {
    return m_type == ParameterType::Float;
}

bool Parameter::is_int() const TANH_NONBLOCKING_FUNCTION {
    return m_type == ParameterType::Int;
}

bool Parameter::is_bool() const TANH_NONBLOCKING_FUNCTION {
    return m_type == ParameterType::Bool;
}

bool Parameter::is_string() const TANH_NONBLOCKING_FUNCTION {
    return m_type == ParameterType::String;
}

// Object-oriented parameter access - renamed to get_from_root
Parameter State::get_from_root(std::string_view key) const {
    return Parameter(this, key);
}

// Parameter notification method
void Parameter::notify(NotifyStrategies strategy, ParameterListener* source) const {
    // Check if this parameter belongs to a group by looking for dots in the
    // path
    std::string path = m_key;
    std::string root_segment = path;
    size_t first_dot = path.find('.');

    // Look up gesture state
    bool is_in_gesture = false;
    m_state->m_index_rcu.read([&](const auto& idx) {
        auto it = idx.find(m_key);
        if (it != idx.end()) {
            is_in_gesture = it->second->m_metadata.m_in_gesture.load(std::memory_order_relaxed);
        }
    });

    if (first_dot != std::string::npos) {
        root_segment = path.substr(0, first_dot);

        // If this parameter belongs to a StateGroup, notify through that group
        // using RCU
        auto* state = const_cast<State*>(m_state);
        StateGroup* found_group = nullptr;

        state->m_groups_rcu.read([&](const auto& groups) {
            auto group_it = groups.find(root_segment);
            if (group_it != groups.end()) { found_group = group_it->second.get(); }
        });

        if (found_group) {
            // This will notify both the group listener and propagate up to the
            // root
            found_group->notify_listeners(path, *this, strategy, source, is_in_gesture);
            return;
        }
    }

    // Otherwise notify directly from State
    const_cast<State*>(m_state)->notify_listeners(path, *this, strategy, source, is_in_gesture);
}

// Get full path for this parameter
std::string Parameter::get_path() const {
    return m_key;
}

std::optional<ParameterDefinition> Parameter::get_definition() const {
    std::lock_guard<std::mutex> lock(m_state->m_storage_mutex);
    ParameterRecord* record = nullptr;
    m_state->m_index_rcu.read([&](const auto& idx) {
        auto it = idx.find(m_key);
        if (it != idx.end()) { record = it->second; }
    });
    if (!record) { return std::nullopt; }
    return record->m_definition;
}

template <typename T>
ParameterHandle<T> Parameter::get_handle() const {
    if (!m_cache_ptr) { throw StateKeyNotFoundException(m_key); }

    constexpr ParameterType k_expected_type = []() {
        if constexpr (std::is_same_v<T, double>) {
            return ParameterType::Double;
        } else if constexpr (std::is_same_v<T, float>) {
            return ParameterType::Float;
        } else if constexpr (std::is_same_v<T, int>) {
            return ParameterType::Int;
        } else if constexpr (std::is_same_v<T, bool>) {
            return ParameterType::Bool;
        }
    }();
    if (m_type != k_expected_type) {
        throw std::invalid_argument(
            "ParameterHandle type mismatch: requested handle type does not match parameter type");
    }

    return ParameterHandle<T>(m_cache_ptr);
}

template ParameterHandle<double> Parameter::get_handle<double>() const;
template ParameterHandle<float> Parameter::get_handle<float>() const;
template ParameterHandle<int> Parameter::get_handle<int>() const;
template ParameterHandle<bool> Parameter::get_handle<bool>() const;

template double Parameter::to<double>(bool allow_blocking) const TANH_NONBLOCKING_FUNCTION;
template float Parameter::to<float>(bool allow_blocking) const TANH_NONBLOCKING_FUNCTION;
template int Parameter::to<int>(bool allow_blocking) const TANH_NONBLOCKING_FUNCTION;
template bool Parameter::to<bool>(bool allow_blocking) const TANH_NONBLOCKING_FUNCTION;
template std::string Parameter::to<std::string>(bool allow_blocking) const
    TANH_NONBLOCKING_FUNCTION;
}  // namespace thl