#include "tanh/state/State.h"

namespace thl {

State::State(size_t max_string_size, size_t max_levels) : m_max_string_size(max_string_size), m_max_levels(max_levels), StateGroup(nullptr, nullptr, ""), m_parameters_rcu(ParameterMap{}) {
    // Initialize the StateGroup with this as the root state
    m_rootState = this;

    // Ensure RCU is initialized for the current thread
    m_parameters_rcu.ensure_thread_registered();
    m_groups_rcu.ensure_thread_registered();
    m_listeners_rcu.ensure_thread_registered();
    
    // Initialize string buffers for real-time safe operations (one-time allocation)
    if (m_path_buffer_1.size() < max_string_size) {
        // Reserve space only if the buffer is not already large enough
        m_path_buffer_1.reserve(max_string_size);
        m_path_buffer_2.reserve(max_string_size);
        m_path_buffer_3.reserve(max_string_size);
        m_temp_buffer.reserve(max_string_size);
        m_path_buffer_1.clear();
        m_path_buffer_2.clear();
        m_path_buffer_3.clear();
        m_temp_buffer.clear();
    }
}

State::~State() {
    // RCU destructors handle cleanup automatically
}

void State::ensure_rcu_initialized() {
    // Ensure RCU is initialized for the current thread
    m_parameters_rcu.ensure_thread_registered();
    m_groups_rcu.ensure_thread_registered();
    m_listeners_rcu.ensure_thread_registered();

    if (m_path_buffer_1.size() < m_max_string_size) {
        // Reserve space only if the buffer is not already large enough
        m_path_buffer_1.reserve(m_max_string_size);
        m_path_buffer_2.reserve(m_max_string_size);
        m_path_buffer_3.reserve(m_max_string_size);
        m_temp_buffer.reserve(m_max_string_size);
        m_path_buffer_1.clear();
        m_path_buffer_2.clear();
        m_path_buffer_3.clear();
        m_temp_buffer.clear();
    }
}

template<typename T>
void State::set_in_root(std::string_view key, T value, bool notify) {
    static_assert(std::is_same_v<T, double> || std::is_same_v<T, float> || std::is_same_v<T, int> || 
                  std::is_same_v<T, bool> || std::is_same_v<T, std::string>, "Unsupported type for set_in_root");
    
    // Try to update existing parameter first (lock-free for numeric types)
    bool parameter_exists = false;
    m_parameters_rcu.update([&](const ParameterMap& params) {
        // Use RCU to update parameter (not real-time safe)
        auto it = params.find(key);
        if (it != params.end()) {
            parameter_exists = true;
            
            // Update atomic values directly for numeric types
            if constexpr (std::is_same_v<T, double>) {
                const_cast<ParameterData&>(it->second).double_value.store(value, std::memory_order_release);
                const_cast<ParameterData&>(it->second).float_value.store(static_cast<float>(value), std::memory_order_release);
                const_cast<ParameterData&>(it->second).int_value.store(static_cast<int>(value), std::memory_order_release);
                const_cast<ParameterData&>(it->second).bool_value.store(value != 0.0, std::memory_order_release);
            } else if constexpr (std::is_same_v<T, float>) {
                const_cast<ParameterData&>(it->second).float_value.store(value, std::memory_order_release);
                const_cast<ParameterData&>(it->second).double_value.store(static_cast<double>(value), std::memory_order_release);
                const_cast<ParameterData&>(it->second).int_value.store(static_cast<int>(value), std::memory_order_release);
                const_cast<ParameterData&>(it->second).bool_value.store(value != 0.0f, std::memory_order_release);
            } else if constexpr (std::is_same_v<T, int>) {
                const_cast<ParameterData&>(it->second).int_value.store(value, std::memory_order_release);
                const_cast<ParameterData&>(it->second).double_value.store(static_cast<double>(value), std::memory_order_release);
                const_cast<ParameterData&>(it->second).float_value.store(static_cast<float>(value), std::memory_order_release);
                const_cast<ParameterData&>(it->second).bool_value.store(value != 0, std::memory_order_release);
            } else if constexpr (std::is_same_v<T, bool>) {
                const_cast<ParameterData&>(it->second).bool_value.store(value, std::memory_order_release);
                const_cast<ParameterData&>(it->second).double_value.store(value ? 1.0 : 0.0, std::memory_order_release);
                const_cast<ParameterData&>(it->second).float_value.store(value ? 1.0f : 0.0f, std::memory_order_release);
                const_cast<ParameterData&>(it->second).int_value.store(value ? 1 : 0, std::memory_order_release);
            } else if constexpr (std::is_same_v<T, std::string>) {
                const_cast<ParameterData&>(it->second).string_value = value;
                
                // Convert string to numeric values
                double d_value = 0.0;
                float f_value = 0.0f;
                int i_value = 0;
                bool b_value = false;
                
                try {
                    if (value == "true" || value == "1" || value == "yes" || value == "on") {
                        b_value = true;
                        d_value = 1.0;
                        f_value = 1.0f;
                        i_value = 1;
                    } else if (value == "false" || value == "0" || value == "no" || value == "off") {
                        b_value = false;
                        d_value = 0.0;
                        f_value = 0.0f;
                        i_value = 0;
                    } else {
                        d_value = std::stod(value);
                        f_value = static_cast<float>(d_value);
                        i_value = static_cast<int>(d_value);
                        b_value = d_value != 0.0;
                    }
                } catch (...) {
                    // Leave default values if conversion fails
                }
                
                // Update atomic values
                const_cast<ParameterData&>(it->second).double_value.store(d_value, std::memory_order_release);
                const_cast<ParameterData&>(it->second).float_value.store(f_value, std::memory_order_release);
                const_cast<ParameterData&>(it->second).int_value.store(i_value, std::memory_order_release);
                const_cast<ParameterData&>(it->second).bool_value.store(b_value, std::memory_order_release);
            }
        }
    });
    
    if (!parameter_exists) {
        // Parameter doesn't exist - use RCU to create it (not real-time safe)
        m_parameters_rcu.update([&](ParameterMap& params) {
            ParameterData new_param;
            
            if constexpr (std::is_same_v<T, double>) {
                new_param.type.store(ParameterType::Double, std::memory_order_release);
                new_param.double_value.store(value, std::memory_order_release);
                new_param.float_value.store(static_cast<float>(value), std::memory_order_release);
                new_param.int_value.store(static_cast<int>(value), std::memory_order_release);
                new_param.bool_value.store(value != 0.0, std::memory_order_release);
            } else if constexpr (std::is_same_v<T, float>) {
                new_param.type.store(ParameterType::Float, std::memory_order_release);
                new_param.float_value.store(value, std::memory_order_release);
                new_param.double_value.store(static_cast<double>(value), std::memory_order_release);
                new_param.int_value.store(static_cast<int>(value), std::memory_order_release);
                new_param.bool_value.store(value != 0.0f, std::memory_order_release);
            } else if constexpr (std::is_same_v<T, int>) {
                new_param.type.store(ParameterType::Int, std::memory_order_release);
                new_param.int_value.store(value, std::memory_order_release);
                new_param.double_value.store(static_cast<double>(value), std::memory_order_release);
                new_param.float_value.store(static_cast<float>(value), std::memory_order_release);
                new_param.bool_value.store(value != 0, std::memory_order_release);
            } else if constexpr (std::is_same_v<T, bool>) {
                new_param.type.store(ParameterType::Bool, std::memory_order_release);
                new_param.bool_value.store(value, std::memory_order_release);
                new_param.double_value.store(value ? 1.0 : 0.0, std::memory_order_release);
                new_param.float_value.store(value ? 1.0f : 0.0f, std::memory_order_release);
                new_param.int_value.store(value ? 1 : 0, std::memory_order_release);
            } else if constexpr (std::is_same_v<T, std::string>) {
                new_param.type.store(ParameterType::String, std::memory_order_release);
                new_param.string_value = value;
                
                // Convert string to numeric values
                double d_value = 0.0;
                float f_value = 0.0f;
                int i_value = 0;
                bool b_value = false;
                
                try {
                    if (value == "true" || value == "1" || value == "yes" || value == "on") {
                        b_value = true;
                        d_value = 1.0;
                        f_value = 1.0f;
                        i_value = 1;
                    } else if (value == "false" || value == "0" || value == "no" || value == "off") {
                        b_value = false;
                        d_value = 0.0;
                        f_value = 0.0f;
                        i_value = 0;
                    } else {
                        d_value = std::stod(value);
                        f_value = static_cast<float>(d_value);
                        i_value = static_cast<int>(d_value);
                        b_value = d_value != 0.0;
                    }
                } catch (...) {
                    // Leave default values if conversion fails
                }
                
                new_param.double_value.store(d_value, std::memory_order_release);
                new_param.float_value.store(f_value, std::memory_order_release);
                new_param.int_value.store(i_value, std::memory_order_release);
                new_param.bool_value.store(b_value, std::memory_order_release);
            }
            
            params[std::string(key)] = std::move(new_param);
        });
    }
    
    // Handle notification
    if (notify) {
        Parameter param_obj(this, key);
        
        auto [group, param_name] = resolve_path(key);
        
        // Notify through the most specific group (which will propagate up)
        // or notify directly from State if no group was found
        if (group) {
            group->notify_listeners(key, param_obj);
        } else {
            const_cast<State*>(this)->notify_listeners(key, param_obj);
        }
    }
}

// Add const char* overload for set_in_root
void State::set_in_root(std::string_view key, const char* value, bool notify) {
    set_in_root(key, std::string(value), notify);
}

// Parameter getters (real-time safe for numeric types)
template<typename T>
T State::get_from_root(std::string_view key) const [[clang::nonblocking]] {
    return m_parameters_rcu.read([&](const ParameterMap& params) -> T {
        auto it = params.find(key);
        if (it == params.end()) {
            throw StateKeyNotFoundException(key);
        }
        
        if constexpr (std::is_same_v<T, double>) {
            return it->second.double_value.load(std::memory_order_acquire);
        } else if constexpr (std::is_same_v<T, float>) {
            return it->second.float_value.load(std::memory_order_acquire);
        } else if constexpr (std::is_same_v<T, int>) {
            return it->second.int_value.load(std::memory_order_acquire);
        } else if constexpr (std::is_same_v<T, bool>) {
            return it->second.bool_value.load(std::memory_order_acquire);
        } else if constexpr (std::is_same_v<T, std::string>) {
            auto type = it->second.type.load(std::memory_order_acquire);
            switch (type) {
                case ParameterType::String: {
                    return it->second.string_value;
                }
                case ParameterType::Double:
                    return std::to_string(it->second.double_value.load(std::memory_order_acquire));
                case ParameterType::Float:
                    return std::to_string(it->second.float_value.load(std::memory_order_acquire));
                case ParameterType::Int:
                    return std::to_string(it->second.int_value.load(std::memory_order_acquire));
                case ParameterType::Bool:
                    return it->second.bool_value.load(std::memory_order_acquire) ? "true" : "false";
                default:
                    return ""; // Unknown type
            }
        }
    });
}

// Get the type of a parameter - renamed to get_type_from_root
ParameterType State::get_type_from_root(std::string_view key) const [[clang::nonblocking]] {
    // Use heterogeneous lookup with string_view directly - no temporary string creation
    return m_parameters_rcu.read([&](const ParameterMap& params) -> ParameterType {
        auto it = params.find(key);
        if (it == params.end()) {
            throw StateKeyNotFoundException(key);
        }

        // Return the stored parameter type
        return it->second.type.load(std::memory_order_acquire);
    });
}

// State management methods
void State::clear() {
    // Clear parameters
    m_parameters_rcu.update([](ParameterMap& params) {
        params.clear();
    });
    
    // Call the base class implementation to clear groups
    StateGroup::clear_groups();
}

bool State::is_empty() const {
    // Check if parameters are empty
    bool parametersEmpty = m_parameters_rcu.read([](const ParameterMap& params) {
        return params.empty();
    });
    
    // If parameters exist, it's not empty
    if (!parametersEmpty) {
        return false;
    }
    
    // Use the base class to check if groups are empty
    return StateGroup::is_empty();
}

// Implementation of update_from_json method
void State::update_from_json(const nlohmann::json& json_data, bool notify) {
    // Helper function to check if a parameter exists before updating
    auto check_parameter_exists = [this](std::string_view key) {
        // Use heterogeneous lookup - no temporary string creation
        if (!m_parameters_rcu.has(key)) {
            throw StateKeyNotFoundException(key);
        }
    };

    // Helper function for recursive update of parameters
    std::function<void(const nlohmann::json&, std::string_view)> update_parameters;
    
    update_parameters = [this, &check_parameter_exists, &update_parameters, &notify](const nlohmann::json& json, std::string_view prefix) {
        for (auto it = json.begin(); it != json.end(); ++it) {
            const std::string& key = it.key();
            std::string path;
            if (prefix.empty()) {
                path = key;
            } else {
                // Use State's pre-allocated buffer
                m_rootState->m_temp_buffer.clear();
                detail::join_path(prefix, key, m_rootState->m_temp_buffer);
                path = m_rootState->m_temp_buffer;
            }
            
            // Check if the value is a nested object
            if (it.value().is_object()) {
                // For nested objects, recurse with updated path
                update_parameters(it.value(), path);
            }
            else {
                check_parameter_exists(path);

                // Update based on JSON type
                if (it.value().is_number_float()) {
                    set(path, it.value().get<double>(), notify, false);
                } 
                else if (it.value().is_number_integer()) {
                    set(path, it.value().get<int>(), notify, false);
                } 
                else if (it.value().is_boolean()) {
                    set(path, it.value().get<bool>(), notify, false);
                } 
                else if (it.value().is_string()) {
                    set(path, it.value().get<std::string>(), notify, false);
                }
                else if (it.value().is_null()) {
                    // Simply log a warning for null values
                    std::cerr << "Warning: Ignoring null value for key '" << path << "'" << std::endl;
                }
            }
        }
    };
    
    try {
        // Start recursive update from root level
        update_parameters(json_data, "");
    }
    catch (const StateKeyNotFoundException& e) {
        // Re-throw the specific exception
        throw;
    }
    catch (const std::exception& e) {
        // Wrap other exceptions
        std::cerr << "Error updating state from JSON: " << e.what() << std::endl;
        throw;
    }
}

template void State::set_in_root(std::string_view key, const double value, bool notify);
template void State::set_in_root(std::string_view key, const float value, bool notify);
template void State::set_in_root(std::string_view key, const int value, bool notify);
template void State::set_in_root(std::string_view key, const bool value, bool notify);
template void State::set_in_root(std::string_view key, const std::string value, bool notify);

template double State::get_from_root(std::string_view key) const [[clang::nonblocking]];
template float State::get_from_root(std::string_view key) const [[clang::nonblocking]];
template int State::get_from_root(std::string_view key) const [[clang::nonblocking]];
template bool State::get_from_root(std::string_view key) const [[clang::nonblocking]];
template std::string State::get_from_root(std::string_view key) const [[clang::nonblocking]];

} // namespace thl
