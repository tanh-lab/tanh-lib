#include "tanh/state/State.h"
#include <tanh/core/Logger.h>

namespace thl {

State::State(size_t max_string_size, size_t max_levels)
    : m_max_string_size(max_string_size)
    , m_max_levels(max_levels)
    , StateGroup(nullptr, nullptr, "")
    , m_index_rcu(IndexMap{}) {
    // Initialize the StateGroup with this as the root state
    m_root_state = this;

    State::ensure_thread_registered();
}

void State::ensure_thread_registered() {
    // Check if this thread is already registered with this State instance
    if (t_registered_states().find(this) != t_registered_states().end()) [[likely]] {
        return;  // Already registered
    }
    register_reader_thread();
    reserve_temporary_string_buffers();
    ensure_child_groups_registered();
    t_registered_states().insert(this);
}

void State::register_reader_thread() {
    // Register this thread with all RCU structures for this State
    m_index_rcu.register_reader_thread();
    m_groups_rcu.register_reader_thread();
    m_listeners_rcu.register_reader_thread();
}

void State::reserve_temporary_string_buffers() {
    // Reserve buffers - no separate tracking needed, called from
    // ensure_thread_registered
    if (m_temp_buffer_0().capacity() < m_max_string_size) {
        // Reserve space only if the buffer is not already large enough
        m_temp_buffer_0().reserve(m_max_string_size);
        m_temp_buffer_1().reserve(m_max_string_size);
        m_temp_buffer_2().reserve(m_max_string_size);
        m_temp_buffer_3().reserve(m_max_string_size);
    }
    m_temp_buffer_0().clear();
    m_temp_buffer_1().clear();
    m_temp_buffer_2().clear();
    m_temp_buffer_3().clear();
}

template <typename T>
void State::set_in_root(std::string_view key,
                        const T& value,
                        NotifyStrategies strategy,
                        ParameterListener* source) {
    static_assert(std::is_same_v<T, double> || std::is_same_v<T, float> || std::is_same_v<T, int> ||
                      std::is_same_v<T, bool> || std::is_same_v<T, std::string>,
                  "Unsupported type for set_in_root");

    // Try to update existing parameter via atomic cache (no RCU copy needed for numerics)
    bool parameter_exists = false;
    ParameterRecord* record = nullptr;

    // Read-only RCU check: does the parameter already exist?
    m_index_rcu.read([&](const IndexMap& idx) {
        auto it = idx.find(key);
        if (it != idx.end()) {
            parameter_exists = true;
            record = it->second;
        }
    });

    if (parameter_exists && record) {
        if constexpr (std::is_same_v<T, std::string>) {
            // Convert string to double — this is the only numeric conversion needed.
            // get_from_root reads from the native atomic and casts on the fly.
            double d_value = 0.0;
            try {
                if (value == "true" || value == "1" || value == "yes" || value == "on") {
                    d_value = 1.0;
                } else if (value == "false" || value == "0" || value == "no" || value == "off") {
                    d_value = 0.0;
                } else {
                    d_value = std::stod(value);
                }
            } catch (...) {  // NOLINT(bugprone-empty-catch) stod may fail on invalid input
                             // default value (0.0) if conversion fails
            }

            if (record->m_type == ParameterType::String) {
                // Only String-typed params store string_value
                {
                    std::scoped_lock lock(m_storage_mutex);
                    record->m_string_value = value;
                }
                // get_from_root reads atomic_double for String-typed numeric access
                record->m_cache.m_atomic_double.store(d_value, std::memory_order_relaxed);
            } else {
                // Non-string param receiving a string value: cast to its native atomic
                switch (record->m_type) {
                    case ParameterType::Double:
                        record->m_cache.m_atomic_double.store(d_value, std::memory_order_relaxed);
                        break;
                    case ParameterType::Float:
                        record->m_cache.m_atomic_float.store(static_cast<float>(d_value),
                                                             std::memory_order_relaxed);
                        break;
                    case ParameterType::Int:
                        record->m_cache.m_atomic_int.store(static_cast<int>(d_value),
                                                           std::memory_order_relaxed);
                        break;
                    case ParameterType::Bool:
                        record->m_cache.m_atomic_bool.store(d_value != 0.0,
                                                            std::memory_order_relaxed);
                        break;
                    default: break;
                }
            }
        } else {
            // Numeric params: skip RCU entirely, convert and write to the parameter's native-type
            // atomic. This handles cross-type sets (e.g., set<int> on a Double-typed param).
            switch (record->m_type) {
                case ParameterType::Double:
                    record->m_cache.m_atomic_double.store(static_cast<double>(value),
                                                          std::memory_order_relaxed);
                    break;
                case ParameterType::Float:
                    record->m_cache.m_atomic_float.store(static_cast<float>(value),
                                                         std::memory_order_relaxed);
                    break;
                case ParameterType::Int:
                    record->m_cache.m_atomic_int.store(static_cast<int>(value),
                                                       std::memory_order_relaxed);
                    break;
                case ParameterType::Bool:
                    if constexpr (std::is_same_v<T, bool>) {
                        record->m_cache.m_atomic_bool.store(value, std::memory_order_relaxed);
                    } else {
                        record->m_cache.m_atomic_bool.store(value != T{0},
                                                            std::memory_order_relaxed);
                    }
                    break;
                case ParameterType::String:
                    // Numeric value to a string-typed param: store in the native numeric atomic
                    // (string_value is not updated — use set<string> for that)
                    record->m_cache.m_atomic_double.store(static_cast<double>(value),
                                                          std::memory_order_relaxed);
                    break;
                default: break;
            }
        }
    } else if (!parameter_exists) {
        // Parameter doesn't exist — create it under the storage mutex
        {
            std::scoped_lock lock(m_storage_mutex);
            auto new_record = std::make_unique<ParameterRecord>();

            if constexpr (std::is_same_v<T, double>) {
                new_record->m_type = ParameterType::Double;
            } else if constexpr (std::is_same_v<T, float>) {
                new_record->m_type = ParameterType::Float;
            } else if constexpr (std::is_same_v<T, int>) {
                new_record->m_type = ParameterType::Int;
            } else if constexpr (std::is_same_v<T, bool>) {
                new_record->m_type = ParameterType::Bool;
            } else if constexpr (std::is_same_v<T, std::string>) {
                new_record->m_type = ParameterType::String;
            }

            // Populate the atomic cache with the native-type value
            if constexpr (std::is_same_v<T, double>) {
                new_record->m_cache.m_atomic_double.store(value, std::memory_order_relaxed);
            } else if constexpr (std::is_same_v<T, float>) {
                new_record->m_cache.m_atomic_float.store(value, std::memory_order_relaxed);
            } else if constexpr (std::is_same_v<T, int>) {
                new_record->m_cache.m_atomic_int.store(value, std::memory_order_relaxed);
            } else if constexpr (std::is_same_v<T, bool>) {
                new_record->m_cache.m_atomic_bool.store(value, std::memory_order_relaxed);
            } else if constexpr (std::is_same_v<T, std::string>) {
                new_record->m_string_value = value;
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
                } catch (...) {  // NOLINT(bugprone-empty-catch) stod may fail on invalid input
                }
                new_record->m_cache.m_atomic_double.store(d_value, std::memory_order_relaxed);
            }

            record = new_record.get();
            m_storage[std::string(key)] = std::move(new_record);
        }

        // Publish the new entry in the lock-free index
        m_index_rcu.update([&](IndexMap& idx) { idx[std::string(key)] = record; });
    }

    // Handle notification
    if (strategy != NotifyStrategies::None) {
        // Copy the key before notifying — key may point into m_temp_buffer_2
        // which re-entrant set() calls from listeners would overwrite.
        std::string key_copy(key);

        Parameter param_obj(this, key_copy);

        bool is_in_gesture =
            record ? record->m_metadata.m_in_gesture.load(std::memory_order_relaxed) : false;

        auto [group, param_name] = resolve_path(key_copy);

        // Notify through the most specific group (which will propagate up)
        // or notify directly from State if no group was found
        if (group) {
            group->notify_listeners(key_copy, param_obj, strategy, source, is_in_gesture);
        } else {
            const_cast<State*>(this)->notify_listeners(key_copy,
                                                       param_obj,
                                                       strategy,
                                                       source,
                                                       is_in_gesture);
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

    // Read type and record pointer from index RCU (lock-free)
    ParameterType param_type = ParameterType::Unknown;
    ParameterRecord* record = nullptr;
    m_index_rcu.read([&](const IndexMap& idx) {
        auto it = idx.find(key);
        if (it == idx.end()) { throw StateKeyNotFoundException(key); }
        param_type = it->second->m_type;
        record = it->second;
    });

    if constexpr (std::is_same_v<T, std::string>) {
        TANH_NONBLOCKING_SCOPED_DISABLER
        switch (param_type) {
            case ParameterType::String: {
                std::scoped_lock lock(m_storage_mutex);
                return record->m_string_value;
            }
            case ParameterType::Double:
                return std::to_string(
                    record->m_cache.m_atomic_double.load(std::memory_order_relaxed));
            case ParameterType::Float:
                return std::to_string(
                    record->m_cache.m_atomic_float.load(std::memory_order_relaxed));
            case ParameterType::Int:
                return std::to_string(record->m_cache.m_atomic_int.load(std::memory_order_relaxed));
            case ParameterType::Bool:
                return record->m_cache.m_atomic_bool.load(std::memory_order_relaxed) ? "true"
                                                                                     : "false";
            default: return "";
        }
    } else {
        // Numeric types: read from the native atomic and convert on the fly
        switch (param_type) {
            case ParameterType::Double:
                return static_cast<T>(
                    record->m_cache.m_atomic_double.load(std::memory_order_relaxed));
            case ParameterType::Float:
                return static_cast<T>(
                    record->m_cache.m_atomic_float.load(std::memory_order_relaxed));
            case ParameterType::Int:
                return static_cast<T>(record->m_cache.m_atomic_int.load(std::memory_order_relaxed));
            case ParameterType::Bool:
                return static_cast<T>(
                    record->m_cache.m_atomic_bool.load(std::memory_order_relaxed));
            case ParameterType::String: {
                // For string-typed params, convert from atomic_double
                return static_cast<T>(
                    record->m_cache.m_atomic_double.load(std::memory_order_relaxed));
            }
            default: return T{};
        }
    }
}

std::string State::get_state_dump(bool include_definitions) const {
    return get_group_state_dump("", include_definitions);
}

std::string State::get_group_state_dump(std::string_view group_prefix,
                                        bool include_definitions) const {
    nlohmann::json root = nlohmann::json::array();

    std::scoped_lock lock(m_storage_mutex);
    for (const auto& [key, record] : m_storage) {
        // If group_prefix is non-empty, skip keys that don't start with it
        if (!group_prefix.empty() &&
            (key.size() < group_prefix.size() || !key.starts_with(group_prefix))) {
            continue;
        }

        nlohmann::json param_obj = nlohmann::json::object();
        param_obj["key"] = key;

        switch (record->m_type) {
            case ParameterType::Double:
                param_obj["value"] =
                    record->m_cache.m_atomic_double.load(std::memory_order_relaxed);
                break;
            case ParameterType::Float:
                param_obj["value"] = record->m_cache.m_atomic_float.load(std::memory_order_relaxed);
                break;
            case ParameterType::Int:
                param_obj["value"] = record->m_cache.m_atomic_int.load(std::memory_order_relaxed);
                break;
            case ParameterType::Bool:
                param_obj["value"] = record->m_cache.m_atomic_bool.load(std::memory_order_relaxed);
                break;
            case ParameterType::String: param_obj["value"] = record->m_string_value; break;
            default: break;
        }

        if (include_definitions && record->m_definition.has_value()) {
            const auto& def = record->m_definition.value();
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

            param_obj["definition"] = def_obj;
        }

        root.push_back(param_obj);
    }

    return root.dump();
}

// Get the type of a parameter - renamed to get_type_from_root
ParameterType State::get_type_from_root(std::string_view key) const TANH_NONBLOCKING_FUNCTION {
    return m_index_rcu.read([&](const IndexMap& idx) -> ParameterType {
        auto it = idx.find(key);
        if (it == idx.end()) { throw StateKeyNotFoundException(key); }
        return it->second->m_type;
    });
}

// State management methods
void State::clear() {
    // Clear the lock-free index first so readers can no longer
    // obtain pointers to records we are about to destroy.
    m_index_rcu.update([](IndexMap& idx) { idx.clear(); });

    // Now safe to destroy the records — all existing ParameterHandles are now invalid
    {
        std::scoped_lock lock(m_storage_mutex);
        m_storage.clear();
    }

    // Call the base class implementation to clear groups
    StateGroup::clear_groups();
}

bool State::is_empty() const TANH_NONBLOCKING_FUNCTION {
    // Check if parameters are empty
    bool parameters_empty = m_index_rcu.read([](const IndexMap& idx) { return idx.empty(); });

    // If parameters exist, it's not empty
    if (!parameters_empty) { return false; }

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
        bool exists =
            m_index_rcu.read([&](const IndexMap& idx) { return idx.find(key) != idx.end(); });
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
                m_root_state->m_temp_buffer_0().clear();
                detail::join_path(prefix, key, m_root_state->m_temp_buffer_0());
                path = m_root_state->m_temp_buffer_0();
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
    ParameterRecord* record = nullptr;
    m_index_rcu.read([&](const IndexMap& idx) {
        auto it = idx.find(key);
        if (it != idx.end()) { record = it->second; }
    });
    if (!record) { throw StateKeyNotFoundException(key); }

    std::scoped_lock lock(m_storage_mutex);
    record->m_definition.emplace(def);
}

std::optional<ParameterDefinition> State::get_definition_from_root(std::string_view key) const {
    ParameterRecord* record = nullptr;
    m_index_rcu.read([&](const IndexMap& idx) {
        auto it = idx.find(key);
        if (it != idx.end()) { record = it->second; }
    });
    if (!record) { throw StateKeyNotFoundException(key); }

    std::scoped_lock lock(m_storage_mutex);
    return record->m_definition;
}

// get_handle - returns a lightweight handle for per-sample real-time access
template <typename T>
ParameterHandle<T> State::get_handle(std::string_view key) const {
    ParameterRecord* record = nullptr;
    m_index_rcu.read([&](const IndexMap& idx) {
        auto it = idx.find(key);
        if (it == idx.end()) { throw StateKeyNotFoundException(key); }
        record = it->second;
    });

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
    if (record->m_type != k_expected_type) {
        throw std::invalid_argument(
            "ParameterHandle type mismatch: requested handle type does not match parameter type");
    }

    return ParameterHandle<T>(&record->m_cache);
}

// set_gesture - sets the gesture state for a parameter
void State::set_gesture(std::string_view key, bool gesture) {
    m_index_rcu.read([&](const IndexMap& idx) {
        auto it = idx.find(key);
        if (it != idx.end()) {
            it->second->m_metadata.m_in_gesture.store(gesture, std::memory_order_relaxed);
        }
    });
}

template void State::set_in_root(std::string_view key,
                                 const double& value,
                                 NotifyStrategies strategy,
                                 ParameterListener* source);
template void State::set_in_root(std::string_view key,
                                 const float& value,
                                 NotifyStrategies strategy,
                                 ParameterListener* source);
template void State::set_in_root(std::string_view key,
                                 const int& value,
                                 NotifyStrategies strategy,
                                 ParameterListener* source);
template void State::set_in_root(std::string_view key,
                                 const bool& value,
                                 NotifyStrategies strategy,
                                 ParameterListener* source);
template void State::set_in_root(std::string_view key,
                                 const std::string& value,
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
