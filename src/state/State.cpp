#include "tanh/state/State.h"
#include <tanh/core/Logger.h>

namespace thl {

State::State(size_t max_string_size, size_t max_levels)
    : m_max_string_size(max_string_size)
    , m_max_levels(max_levels)
    , StateGroup(nullptr, nullptr, "")
    , m_parameters_rcu(ParameterMap{})
    , m_strings_rcu(StringMap{})
    , m_definitions_rcu(DefinitionMap{}) {
    // Initialize the StateGroup with this as the root state
    m_rootState = this;

    ensure_thread_registered();
}

void State::ensure_thread_registered() {
    // Check if this thread is already registered with this State instance
    if (t_registered_states.find(this) != t_registered_states.end()) [[likely]] {
        return;  // Already registered
    }
    register_reader_thread();
    reserve_temporary_string_buffers();
    ensure_child_groups_registered();
    t_registered_states.insert(this);
}

void State::register_reader_thread() {
    // Register this thread with all RCU structures for this State
    m_parameters_rcu.register_reader_thread();
    m_strings_rcu.register_reader_thread();
    m_definitions_rcu.register_reader_thread();
    m_groups_rcu.register_reader_thread();
    m_listeners_rcu.register_reader_thread();
}

void State::reserve_temporary_string_buffers() {
    // Reserve buffers - no separate tracking needed, called from
    // ensure_thread_registered
    if (m_temp_buffer_0.capacity() < m_max_string_size) {
        // Reserve space only if the buffer is not already large enough
        m_temp_buffer_0.reserve(m_max_string_size);
        m_temp_buffer_1.reserve(m_max_string_size);
        m_temp_buffer_2.reserve(m_max_string_size);
        m_temp_buffer_3.reserve(m_max_string_size);
    }
    m_temp_buffer_0.clear();
    m_temp_buffer_1.clear();
    m_temp_buffer_2.clear();
    m_temp_buffer_3.clear();
}

template <typename T>
void State::set_in_root(std::string_view key,
                        T value,
                        NotifyStrategies strategy,
                        ParameterListener* source) {
    static_assert(std::is_same_v<T, double> || std::is_same_v<T, float> || std::is_same_v<T, int> ||
                      std::is_same_v<T, bool> || std::is_same_v<T, std::string>,
                  "Unsupported type for set_in_root");

    // Try to update existing parameter via atomic cache (no RCU copy needed for numerics)
    bool parameter_exists = false;
    AtomicCacheEntry* existing_cache = nullptr;
    ParameterType existing_type = ParameterType::Unknown;

    // Read-only RCU check: does the parameter already exist?
    m_parameters_rcu.read([&](const ParameterMap& params) {
        auto it = params.find(key);
        if (it != params.end()) {
            parameter_exists = true;
            existing_cache = it->second.cache_ptr;
            existing_type = it->second.type;
        }
    });

    if (parameter_exists && existing_cache) {
        if constexpr (std::is_same_v<T, std::string>) {
            // Convert string to double — this is the only numeric conversion needed.
            // get_from_root reads from the native atomic and casts on the fly.
            double d_value = 0.0;
            try {
                if (value == "true" || value == "1" || value == "yes" || value == "on") {
                    d_value = 1.0;
                } else if (value == "false" || value == "0" || value == "no" ||
                           value == "off") {
                    d_value = 0.0;
                } else {
                    d_value = std::stod(value);
                }
            } catch (...) {
                // Leave default value (0.0) if conversion fails
            }

            if (existing_type == ParameterType::String) {
                // Only String-typed params store string_value (via strings RCU)
                m_strings_rcu.update([&](StringMap& strings) {
                    strings[std::string(key)] = value;
                });
                // get_from_root reads atomic_double for String-typed numeric access
                existing_cache->atomic_double.store(d_value, std::memory_order_relaxed);
            } else {
                // Non-string param receiving a string value: cast to its native atomic
                switch (existing_type) {
                    case ParameterType::Double:
                        existing_cache->atomic_double.store(d_value, std::memory_order_relaxed);
                        break;
                    case ParameterType::Float:
                        existing_cache->atomic_float.store(static_cast<float>(d_value),
                                                           std::memory_order_relaxed);
                        break;
                    case ParameterType::Int:
                        existing_cache->atomic_int.store(static_cast<int>(d_value),
                                                         std::memory_order_relaxed);
                        break;
                    case ParameterType::Bool:
                        existing_cache->atomic_bool.store(d_value != 0.0,
                                                          std::memory_order_relaxed);
                        break;
                    default: break;
                }
            }
        } else {
            // Numeric params: skip RCU entirely, convert and write to the parameter's native-type
            // atomic. This handles cross-type sets (e.g., set<int> on a Double-typed param).
            switch (existing_type) {
                case ParameterType::Double:
                    existing_cache->atomic_double.store(static_cast<double>(value),
                                                        std::memory_order_relaxed);
                    break;
                case ParameterType::Float:
                    existing_cache->atomic_float.store(static_cast<float>(value),
                                                       std::memory_order_relaxed);
                    break;
                case ParameterType::Int:
                    existing_cache->atomic_int.store(static_cast<int>(value),
                                                     std::memory_order_relaxed);
                    break;
                case ParameterType::Bool:
                    if constexpr (std::is_same_v<T, bool>) {
                        existing_cache->atomic_bool.store(value, std::memory_order_relaxed);
                    } else {
                        existing_cache->atomic_bool.store(value != T{0},
                                                          std::memory_order_relaxed);
                    }
                    break;
                case ParameterType::String:
                    // Numeric value to a string-typed param: store in the native numeric atomic
                    // (string_value is not updated — use set<string> for that)
                    existing_cache->atomic_double.store(static_cast<double>(value),
                                                        std::memory_order_relaxed);
                    break;
                default: break;
            }
        }
    } else if (!parameter_exists) {
        // Create atomic cache entry for the new parameter
        AtomicCacheEntry* cache_entry = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_cache_mutex);
            auto [cache_it, inserted] =
                m_atomic_cache.emplace(std::string(key), std::make_unique<AtomicCacheEntry>());
            cache_entry = cache_it->second.get();
        }

        // Parameter doesn't exist - use RCU to create it
        m_parameters_rcu.update([&](ParameterMap& params) {
            ParameterRecord new_param;

            if constexpr (std::is_same_v<T, double>) {
                new_param.type = ParameterType::Double;
            } else if constexpr (std::is_same_v<T, float>) {
                new_param.type = ParameterType::Float;
            } else if constexpr (std::is_same_v<T, int>) {
                new_param.type = ParameterType::Int;
            } else if constexpr (std::is_same_v<T, bool>) {
                new_param.type = ParameterType::Bool;
            } else if constexpr (std::is_same_v<T, std::string>) {
                new_param.type = ParameterType::String;
            }

            // Link to atomic cache
            new_param.cache_ptr = cache_entry;
            params[std::string(key)] = std::move(new_param);
        });

        // String values go into the separate strings RCU
        if constexpr (std::is_same_v<T, std::string>) {
            m_strings_rcu.update([&](StringMap& strings) {
                strings[std::string(key)] = value;
            });
        }

        // Populate the atomic cache with the native-type value
        if constexpr (std::is_same_v<T, double>) {
            cache_entry->atomic_double.store(value, std::memory_order_relaxed);
        } else if constexpr (std::is_same_v<T, float>) {
            cache_entry->atomic_float.store(value, std::memory_order_relaxed);
        } else if constexpr (std::is_same_v<T, int>) {
            cache_entry->atomic_int.store(value, std::memory_order_relaxed);
        } else if constexpr (std::is_same_v<T, bool>) {
            cache_entry->atomic_bool.store(value, std::memory_order_relaxed);
        } else if constexpr (std::is_same_v<T, std::string>) {
            // Only atomic_double is needed for String-typed params because
            // get_from_root reads exclusively from atomic_double and casts to
            // the requested type T on the fly via static_cast<T>.
            double d_value = 0.0;
            try {
                if (value == "true" || value == "1" || value == "yes" || value == "on") {
                    d_value = 1.0;
                } else if (value == "false" || value == "0" || value == "no" ||
                           value == "off") {
                    d_value = 0.0;
                } else {
                    d_value = std::stod(value);
                }
            } catch (...) {
                // Leave default value (0.0) if conversion fails
            }

            cache_entry->atomic_double.store(d_value, std::memory_order_relaxed);
        }
    }

    // Handle notification
    if (strategy != NotifyStrategies::none) {
        // Copy the key before notifying — key may point into m_temp_buffer_2
        // which re-entrant set() calls from listeners would overwrite.
        std::string key_copy(key);

        Parameter param_obj(this, key_copy);

        auto [group, param_name] = resolve_path(key_copy);

        // Notify through the most specific group (which will propagate up)
        // or notify directly from State if no group was found
        if (group) {
            group->notify_listeners(key_copy, param_obj, strategy, source);
        } else {
            const_cast<State*>(this)->notify_listeners(key_copy, param_obj, strategy, source);
        }
    }
}

// Add const char* overload for set_in_root
void State::set_in_root(std::string_view key,
                        const char* value,
                        NotifyStrategies strategy,
                        ParameterListener* source) {
    set_in_root(key, std::string(value), strategy, source);
}

// Parameter getters (real-time safe for numeric types)
template <typename T>
T State::get_from_root(std::string_view key, bool allow_blocking) const TANH_NONBLOCKING_FUNCTION {
    // String access requires allow_blocking=true as it may allocate memory
    if constexpr (std::is_same_v<T, std::string>) {
        if (!allow_blocking) { throw BlockingException(key); }
    }

    // Read type and cache_ptr from parameters RCU (lock-free)
    ParameterType param_type = ParameterType::Unknown;
    AtomicCacheEntry* cache = nullptr;
    m_parameters_rcu.read([&](const ParameterMap& params) {
        auto it = params.find(key);
        if (it == params.end()) { throw StateKeyNotFoundException(key); }
        param_type = it->second.type;
        cache = it->second.cache_ptr;
    });

    if constexpr (std::is_same_v<T, std::string>) {
        TANH_NONBLOCKING_SCOPED_DISABLER
        switch (param_type) {
            case ParameterType::String:
                return m_strings_rcu.read([&](const StringMap& strings) -> std::string {
                    auto it = strings.find(key);
                    return it != strings.end() ? it->second : "";
                });
            case ParameterType::Double:
                return std::to_string(
                    cache->atomic_double.load(std::memory_order_relaxed));
            case ParameterType::Float:
                return std::to_string(
                    cache->atomic_float.load(std::memory_order_relaxed));
            case ParameterType::Int:
                return std::to_string(
                    cache->atomic_int.load(std::memory_order_relaxed));
            case ParameterType::Bool:
                return cache->atomic_bool.load(std::memory_order_relaxed) ? "true"
                                                                         : "false";
            default: return "";
        }
    } else {
        // Numeric types: read from the native atomic and convert on the fly
        switch (param_type) {
            case ParameterType::Double:
                return static_cast<T>(
                    cache->atomic_double.load(std::memory_order_relaxed));
            case ParameterType::Float:
                return static_cast<T>(
                    cache->atomic_float.load(std::memory_order_relaxed));
            case ParameterType::Int:
                return static_cast<T>(
                    cache->atomic_int.load(std::memory_order_relaxed));
            case ParameterType::Bool:
                return static_cast<T>(
                    cache->atomic_bool.load(std::memory_order_relaxed));
            case ParameterType::String: {
                // For string-typed params, convert from atomic_double
                return static_cast<T>(
                    cache->atomic_double.load(std::memory_order_relaxed));
            }
            default: return T{};
        }
    }
}

std::string State::get_state_dump() const {
    nlohmann::json root = nlohmann::json::array();

    // Snapshot string values
    StringMap strings_snap;
    m_strings_rcu.read([&](const StringMap& s) { strings_snap = s; });

    // Build parameter entries from the parameters RCU
    m_parameters_rcu.read([&](const ParameterMap& params) {
        for (const auto& [key, data] : params) {
            nlohmann::json param_obj = nlohmann::json::object();
            param_obj["key"] = key;

            // Get current value based on type
            auto type = data.type;
            if (type == ParameterType::Double) {
                param_obj["value"] =
                    data.cache_ptr->atomic_double.load(std::memory_order_relaxed);
            } else if (type == ParameterType::Float) {
                param_obj["value"] =
                    data.cache_ptr->atomic_float.load(std::memory_order_relaxed);
            } else if (type == ParameterType::Int) {
                param_obj["value"] = data.cache_ptr->atomic_int.load(std::memory_order_relaxed);
            } else if (type == ParameterType::Bool) {
                param_obj["value"] =
                    data.cache_ptr->atomic_bool.load(std::memory_order_relaxed);
            } else if (type == ParameterType::String) {
                auto sit = strings_snap.find(key);
                param_obj["value"] = sit != strings_snap.end() ? sit->second : "";
            }

            root.push_back(param_obj);
        }
    });

    // Add definitions from the definitions RCU
    m_definitions_rcu.read([&](const DefinitionMap& defs) {
        for (auto& elem : root) {
            std::string key = elem["key"].get<std::string>();
            auto it = defs.find(key);
            if (it == defs.end()) continue;

            const auto& def = it->second;
            nlohmann::json def_obj = nlohmann::json::object();

            def_obj["name"] = def.m_name;
            def_obj["type"] = [&]() {
                switch (def.m_type) {
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

            def_obj["slider_polarity"] = [&]() {
                switch (def.m_slider_polarity) {
                    case SliderPolarity::Unipolar: return "unipolar";
                    case SliderPolarity::Bipolar: return "bipolar";
                    default: return "unipolar";
                }
            }();

            if (!def.m_data.empty()) { def_obj["data"] = def.m_data; }

            elem["definition"] = def_obj;
        }
    });

    return root.dump();
}

// Get the type of a parameter - renamed to get_type_from_root
ParameterType State::get_type_from_root(std::string_view key) const TANH_NONBLOCKING_FUNCTION {
    // Use heterogeneous lookup with string_view directly - no temporary string
    // creation
    return m_parameters_rcu.read([&](const ParameterMap& params) -> ParameterType {
        auto it = params.find(key);
        if (it == params.end()) { throw StateKeyNotFoundException(key); }

        // Return the stored parameter type
        return it->second.type;
    });
}

// State management methods
void State::clear() {
    // Clear parameters
    m_parameters_rcu.update([](ParameterMap& params) { params.clear(); });

    // Clear string values
    m_strings_rcu.update([](StringMap& strings) { strings.clear(); });

    // Clear definitions
    m_definitions_rcu.update([](DefinitionMap& defs) { defs.clear(); });

    // Clear the atomic cache — all existing ParameterHandles are now invalid
    {
        std::lock_guard<std::mutex> lock(m_cache_mutex);
        m_atomic_cache.clear();
    }

    // Call the base class implementation to clear groups
    StateGroup::clear_groups();
}

bool State::is_empty() const TANH_NONBLOCKING_FUNCTION {
    // Check if parameters are empty
    bool parametersEmpty =
        m_parameters_rcu.read([](const ParameterMap& params) { return params.empty(); });

    // If parameters exist, it's not empty
    if (!parametersEmpty) { return false; }

    // Use the base class to check if groups are empty
    return StateGroup::is_empty();
}

// Implementation of update_from_json method
void State::update_from_json(const nlohmann::json& json_data,
                             NotifyStrategies strategy,
                             ParameterListener* source) {
    // Ensure thread is registered for RT-safe access
    ensure_thread_registered();

    // Helper function to check if a parameter exists before updating
    auto check_parameter_exists = [this](std::string_view key) {
        bool exists = m_parameters_rcu.read(
            [&](const ParameterMap& params) { return params.find(key) != params.end(); });
        if (!exists) { throw StateKeyNotFoundException(key); }
    };

    // Helper function for recursive update of parameters
    std::function<void(const nlohmann::json&, std::string_view)> update_parameters;

    update_parameters = [this, &check_parameter_exists, &update_parameters, &strategy, &source](
                            const nlohmann::json& json,
                            std::string_view prefix) {
        for (auto it = json.begin(); it != json.end(); ++it) {
            const std::string& key = it.key();
            std::string path;
            if (prefix.empty()) {
                path = key;
            } else {
                // Use State's pre-allocated buffer
                m_rootState->m_temp_buffer_0.clear();
                detail::join_path(prefix, key, m_rootState->m_temp_buffer_0);
                path = m_rootState->m_temp_buffer_0;
            }

            // Check if the value is a nested object
            if (it.value().is_object()) {
                // For nested objects, recurse with updated path
                update_parameters(it.value(), path);
            } else {
                check_parameter_exists(path);

                // Update based on JSON type
                if (it.value().is_number_float()) {
                    set(path, it.value().get<double>(), strategy, source, false);
                } else if (it.value().is_number_integer()) {
                    set(path, it.value().get<int>(), strategy, source, false);
                } else if (it.value().is_boolean()) {
                    set(path, it.value().get<bool>(), strategy, source, false);
                } else if (it.value().is_string()) {
                    set(path, it.value().get<std::string>(), strategy, source, false);
                } else if (it.value().is_null()) {
                    // Simply log a warning for null values
                    thl::Logger::logf(thl::Logger::LogLevel::Warning,
                                      "thl.state.state",
                                      "Ignoring null value for key '%s'",
                                      path.c_str());
                }
            }
        }
    };

    try {
        // Start recursive update from root level
        update_parameters(json_data, "");
    } catch (const StateKeyNotFoundException& e) {
        // Re-throw the specific exception
        throw;
    } catch (const std::exception& e) {
        // Wrap other exceptions
        thl::Logger::logf(thl::Logger::LogLevel::Error,
                          "thl.state.state",
                          "Error updating state from JSON: %s",
                          e.what());
        throw;
    }
}

// Parameter definition management
void State::set_definition_in_root(std::string_view key, const ParameterDefinition& def) {
    // Verify param exists
    bool exists = m_parameters_rcu.read([&](const ParameterMap& params) {
        return params.find(key) != params.end();
    });
    if (!exists) { throw StateKeyNotFoundException(key); }

    m_definitions_rcu.update([&](DefinitionMap& defs) {
        defs.erase(std::string(key));
        defs.emplace(std::string(key), def);
    });
}

std::optional<ParameterDefinition> State::get_definition_from_root(std::string_view key) const {
    // Verify param exists
    bool exists = m_parameters_rcu.read([&](const ParameterMap& params) {
        return params.find(key) != params.end();
    });
    if (!exists) { throw StateKeyNotFoundException(key); }

    return m_definitions_rcu.read([&](const DefinitionMap& defs) -> std::optional<ParameterDefinition> {
        auto it = defs.find(key);
        if (it != defs.end()) {
            return it->second;
        }
        return std::nullopt;
    });
}

// get_handle - returns a lightweight handle for per-sample real-time access
template <typename T>
ParameterHandle<T> State::get_handle(std::string_view key) const {
    // Verify the parameter's type matches T
    ParameterType param_type = get_type_from_root(key);
    constexpr ParameterType expected_type = []() {
        if constexpr (std::is_same_v<T, double>) return ParameterType::Double;
        else if constexpr (std::is_same_v<T, float>) return ParameterType::Float;
        else if constexpr (std::is_same_v<T, int>) return ParameterType::Int;
        else if constexpr (std::is_same_v<T, bool>) return ParameterType::Bool;
    }();
    if (param_type != expected_type) {
        throw std::invalid_argument(
            "ParameterHandle type mismatch: requested handle type does not match parameter type");
    }

    std::lock_guard<std::mutex> lock(m_cache_mutex);
    auto it = m_atomic_cache.find(key);
    if (it == m_atomic_cache.end()) { throw StateKeyNotFoundException(key); }
    return ParameterHandle<T>(it->second.get());
}

template void State::set_in_root(std::string_view key,
                                 const double value,
                                 NotifyStrategies strategy,
                                 ParameterListener* source);
template void State::set_in_root(std::string_view key,
                                 const float value,
                                 NotifyStrategies strategy,
                                 ParameterListener* source);
template void State::set_in_root(std::string_view key,
                                 const int value,
                                 NotifyStrategies strategy,
                                 ParameterListener* source);
template void State::set_in_root(std::string_view key,
                                 const bool value,
                                 NotifyStrategies strategy,
                                 ParameterListener* source);
template void State::set_in_root(std::string_view key,
                                 const std::string value,
                                 NotifyStrategies strategy,
                                 ParameterListener* source);

template double State::get_from_root(std::string_view key,
                                     bool allow_blocking) const TANH_NONBLOCKING_FUNCTION;
template float State::get_from_root(std::string_view key,
                                    bool allow_blocking) const TANH_NONBLOCKING_FUNCTION;
template int State::get_from_root(std::string_view key,
                                  bool allow_blocking) const TANH_NONBLOCKING_FUNCTION;
template bool State::get_from_root(std::string_view key,
                                   bool allow_blocking) const TANH_NONBLOCKING_FUNCTION;
template std::string State::get_from_root(std::string_view key,
                                          bool allow_blocking) const TANH_NONBLOCKING_FUNCTION;

template ParameterHandle<double> State::get_handle(std::string_view key) const;
template ParameterHandle<float> State::get_handle(std::string_view key) const;
template ParameterHandle<int> State::get_handle(std::string_view key) const;
template ParameterHandle<bool> State::get_handle(std::string_view key) const;
}  // namespace thl
