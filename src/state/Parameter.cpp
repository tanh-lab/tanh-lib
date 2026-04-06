#include <tanh/state/Parameter.h>
#include <tanh/state/State.h>
#include <tanh/utils/RealtimeSanitizer.h>
#include <cstddef>
#include <string_view>
#include "tanh/state/StateGroup.h"
#include <mutex>
#include <string>
#include <atomic>
#include "tanh/state/ParameterDefinitions.h"
#include <cstdint>
#include "tanh/core/Exports.h"
#include "tanh/state/Exceptions.h"
#include "tanh/state/ParameterListener.h"

namespace thl {

// Parameter constructor
Parameter::Parameter(const State* state, ParameterRecord* record)
    : m_state(state), m_record(record) {}

// Parameter conversion methods
template <typename T>
T Parameter::to(bool allow_blocking) const TANH_NONBLOCKING_FUNCTION {
    if constexpr (std::is_same_v<T, std::string>) {
        if (!allow_blocking) { throw BlockingException(m_record->m_key); }
        TANH_NONBLOCKING_SCOPED_DISABLER
        switch (m_record->m_def.m_type) {
            case ParameterType::String: {
                std::scoped_lock const lock(m_state->m_storage_mutex);
                return m_record->m_string_value;
            }
            case ParameterType::Double:
                return std::to_string(
                    m_record->m_cache.m_atomic_double.load(std::memory_order_relaxed));
            case ParameterType::Float:
                return std::to_string(
                    m_record->m_cache.m_atomic_float.load(std::memory_order_relaxed));
            case ParameterType::Int:
                return std::to_string(
                    m_record->m_cache.m_atomic_int.load(std::memory_order_relaxed));
            case ParameterType::Bool:
                return m_record->m_cache.m_atomic_bool.load(std::memory_order_relaxed) ? "true"
                                                                                       : "false";
            default: return "";
        }
    } else {
        switch (m_record->m_def.m_type) {
            case ParameterType::Double:
                return static_cast<T>(
                    m_record->m_cache.m_atomic_double.load(std::memory_order_relaxed));
            case ParameterType::Float:
                return static_cast<T>(
                    m_record->m_cache.m_atomic_float.load(std::memory_order_relaxed));
            case ParameterType::Int:
                return static_cast<T>(
                    m_record->m_cache.m_atomic_int.load(std::memory_order_relaxed));
            case ParameterType::Bool:
                return static_cast<T>(
                    m_record->m_cache.m_atomic_bool.load(std::memory_order_relaxed));
            case ParameterType::String:
                return static_cast<T>(
                    m_record->m_cache.m_atomic_double.load(std::memory_order_relaxed));
            default: return T{};
        }
    }
}

// Parameter type checking — reads from immutable m_def
ParameterType Parameter::get_type() const TANH_NONBLOCKING_FUNCTION {
    return m_record->m_def.m_type;
}

bool Parameter::is_double() const TANH_NONBLOCKING_FUNCTION {
    return m_record->m_def.m_type == ParameterType::Double;
}

bool Parameter::is_float() const TANH_NONBLOCKING_FUNCTION {
    return m_record->m_def.m_type == ParameterType::Float;
}

bool Parameter::is_int() const TANH_NONBLOCKING_FUNCTION {
    return m_record->m_def.m_type == ParameterType::Int;
}

bool Parameter::is_bool() const TANH_NONBLOCKING_FUNCTION {
    return m_record->m_def.m_type == ParameterType::Bool;
}

bool Parameter::is_string() const TANH_NONBLOCKING_FUNCTION {
    return m_record->m_def.m_type == ParameterType::String;
}

// Metadata accessors — forward to immutable m_def
std::string_view Parameter::key() const TANH_NONBLOCKING_FUNCTION {
    return m_record->m_key;
}

const ParameterDefinition& Parameter::def() const TANH_NONBLOCKING_FUNCTION {
    return m_record->m_def;
}

const Range& Parameter::range() const TANH_NONBLOCKING_FUNCTION {
    return m_record->m_def.m_range;
}

uint32_t Parameter::id() const TANH_NONBLOCKING_FUNCTION {
    return m_record->m_def.m_id;
}

// Parameter notification method
void Parameter::notify(ParameterListener* source) const {
    std::string_view const param_key = m_record->m_key;
    std::string const key_copy(param_key);

    size_t const first_dot = key_copy.find('.');

    if (first_dot != std::string::npos) {
        std::string root_segment = key_copy.substr(0, first_dot);

        auto* state = const_cast<State*>(m_state);
        StateGroup* found_group = nullptr;

        state->m_groups_rcu.read([&](const auto& groups) {
            auto group_it = groups.find(root_segment);
            if (group_it != groups.end()) { found_group = group_it->second.get(); }
        });

        if (found_group) {
            found_group->notify_listeners(*this, source);
            return;
        }
    }

    const_cast<State*>(m_state)->notify_listeners(*this, source);
}

// ParameterHandle from Parameter
template <typename T>
ParameterHandle<T> Parameter::get_handle() const {
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
    if (m_record->m_def.m_type != k_expected_type) {
        throw ParameterTypeMismatchException(k_expected_type, m_record->m_def.m_type);
    }

    return ParameterHandle<T>(m_record);
}

template TANH_API ParameterHandle<double> Parameter::get_handle<double>() const;
template TANH_API ParameterHandle<float> Parameter::get_handle<float>() const;
template TANH_API ParameterHandle<int> Parameter::get_handle<int>() const;
template TANH_API ParameterHandle<bool> Parameter::get_handle<bool>() const;

template TANH_API double Parameter::to<double>(bool allow_blocking) const TANH_NONBLOCKING_FUNCTION;
template TANH_API float Parameter::to<float>(bool allow_blocking) const TANH_NONBLOCKING_FUNCTION;
template TANH_API int Parameter::to<int>(bool allow_blocking) const TANH_NONBLOCKING_FUNCTION;
template TANH_API bool Parameter::to<bool>(bool allow_blocking) const TANH_NONBLOCKING_FUNCTION;
template TANH_API std::string Parameter::to<std::string>(bool allow_blocking) const
    TANH_NONBLOCKING_FUNCTION;
}  // namespace thl
