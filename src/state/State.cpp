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

    thread_registered = true;
}

State::~State() {
    // RCU destructors handle cleanup automatically
}

void State::ensure_rcu_initialized() {
    if (!thread_registered) [[ unlikely ]] {   
#ifdef TANH_WITH_RTSAN
        __rtsan::ScopedDisabler sd; // TODO: Find a better solution
#endif
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
        thread_registered = true;
    }
}

template<typename T>
void State::set_in_root(std::string_view key, T value, NotifyStrategies strategy, ParameterListener* source) {
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
    if (strategy != NotifyStrategies::none) {
        Parameter param_obj(this, key);
        
        auto [group, param_name] = resolve_path(key);
        
        // Notify through the most specific group (which will propagate up)
        // or notify directly from State if no group was found
        if (group) {
            group->notify_listeners(key, param_obj, strategy, source);
        } else {
            const_cast<State*>(this)->notify_listeners(key, param_obj, strategy, source);
        }
    }
}

// Add const char* overload for set_in_root
void State::set_in_root(std::string_view key, const char* value, NotifyStrategies strategy, ParameterListener* source) {
    set_in_root(key, std::string(value), strategy, source);
}

// Parameter getters (real-time safe for numeric types)
template<typename T>
T State::get_from_root(std::string_view key) const TANH_NONBLOCKING {
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
#ifdef TANH_WITH_RTSAN
            __rtsan::ScopedDisabler sd;
#endif
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

std::string State::get_state_dump() const {
  nlohmann::json root = nlohmann::json::array();

  m_parameters_rcu.read([&](const ParameterMap &params) {
    for (const auto &[key, data] : params) {
      nlohmann::json param_obj = nlohmann::json::object();
      param_obj["key"] = key;
      
      // Get current value based on type
      auto type = data.type.load(std::memory_order_relaxed);
      if (type == ParameterType::Double) {
        param_obj["value"] = data.double_value.load(std::memory_order_relaxed);
      } else if (type == ParameterType::Float) {
        param_obj["value"] = data.float_value.load(std::memory_order_relaxed);
      } else if (type == ParameterType::Int) {
        param_obj["value"] = data.int_value.load(std::memory_order_relaxed);
      } else if (type == ParameterType::Bool) {
        param_obj["value"] = data.bool_value.load(std::memory_order_relaxed);
      } else if (type == ParameterType::String) {
        param_obj["value"] = data.string_value;
      }
      
      // Include definition if it exists
      if (data.parameter_definition) {
        nlohmann::json def_obj = nlohmann::json::object();
        const auto& def = *data.parameter_definition;
        
        def_obj["name"] = def.m_name;
        def_obj["type"] = [&]() {
          switch(def.m_type) {
            case PluginParamType::ParamFloat: return "float";
            case PluginParamType::ParamInt: return "int";
            case PluginParamType::ParamBool: return "bool";
            case PluginParamType::ParamChoice: return "choice";
            default: return "unknown";
          }
        }();
        
        // Range information
        def_obj["min"] = def.m_range.m_min;
        def_obj["max"] = def.m_range.m_max;
        def_obj["step"] = def.m_range.m_step;
        def_obj["skew"] = def.m_range.m_skew;
        
        def_obj["default_value"] = def.m_default_value;
        def_obj["decimal_places"] = def.m_decimal_places;
        def_obj["automation"] = def.m_automation;
        def_obj["modulation"] = def.m_modulation;
        
        // Include choice data if available
        if (!def.m_data.empty()) {
          def_obj["data"] = def.m_data;
        }
        
        param_obj["definition"] = def_obj;
      }
      
      root.push_back(param_obj);
    }
  });

  return root.dump();
}

// Get the type of a parameter - renamed to get_type_from_root
ParameterType State::get_type_from_root(std::string_view key) const TANH_NONBLOCKING {
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
void State::update_from_json(const nlohmann::json& json_data, NotifyStrategies strategy, ParameterListener* source) {
    // Helper function to check if a parameter exists before updating
    auto check_parameter_exists = [this](std::string_view key) {
        // Use heterogeneous lookup - no temporary string creation
        if (!m_parameters_rcu.has(key)) {
            throw StateKeyNotFoundException(key);
        }
    };

    // Helper function for recursive update of parameters
    std::function<void(const nlohmann::json&, std::string_view)> update_parameters;

    update_parameters = [this, &check_parameter_exists, &update_parameters, &strategy, &source](const nlohmann::json& json, std::string_view prefix) {
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
                    set(path, it.value().get<double>(), strategy, source, false);
                } 
                else if (it.value().is_number_integer()) {
                    set(path, it.value().get<int>(), strategy, source, false);
                } 
                else if (it.value().is_boolean()) {
                    set(path, it.value().get<bool>(), strategy, source, false);
                } 
                else if (it.value().is_string()) {
                    set(path, it.value().get<std::string>(), strategy, source, false);
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
#ifdef TANH_WITH_RTSAN
        __rtsan::ScopedDisabler sd; // TODO: Find a better solution
#endif
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

// Parameter definition management
void State::set_definition_in_root(std::string_view key, const ParameterDefinition& def) {
    m_parameters_rcu.update([&](const ParameterMap& params) {
        auto it = params.find(key);
        if (it != params.end()) {
            const_cast<ParameterData&>(it->second).parameter_definition =
                std::make_unique<ParameterDefinition>(def);
        } else {
            throw StateKeyNotFoundException(key);
        }
    });
}

ParameterDefinition* State::get_definition_from_root(std::string_view key) const {
    return m_parameters_rcu.read([&](const ParameterMap& params) -> ParameterDefinition* {
        auto it = params.find(key);
        if (it == params.end()) {
            throw StateKeyNotFoundException(key);
        }
        return it->second.parameter_definition.get();
    });
}

template void State::set_in_root(std::string_view key, const double value, NotifyStrategies strategy, ParameterListener* source);
template void State::set_in_root(std::string_view key, const float value, NotifyStrategies strategy, ParameterListener* source);
template void State::set_in_root(std::string_view key, const int value, NotifyStrategies strategy, ParameterListener* source);
template void State::set_in_root(std::string_view key, const bool value, NotifyStrategies strategy, ParameterListener* source);
template void State::set_in_root(std::string_view key, const std::string value, NotifyStrategies strategy, ParameterListener* source);

template double State::get_from_root(std::string_view key) const TANH_NONBLOCKING;
template float State::get_from_root(std::string_view key) const TANH_NONBLOCKING;
template int State::get_from_root(std::string_view key) const TANH_NONBLOCKING;
template bool State::get_from_root(std::string_view key) const TANH_NONBLOCKING;
template std::string State::get_from_root(std::string_view key) const TANH_NONBLOCKING;

} // namespace thl
