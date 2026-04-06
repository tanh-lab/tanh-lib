#include "tanh/state/State.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include <nlohmann/json_fwd.hpp>
#include <tanh/core/Logger.h>

#include "tanh/core/Exports.h"
#include "tanh/state/Parameter.h"
#include "tanh/state/ParameterDefinitions.h"
#include "tanh/state/StateGroup.h"
#include "tanh/state/path_helpers.h"
#include "tanh/utils/RealtimeSanitizer.h"
#include "tanh/state/Exceptions.h"
#include "tanh/state/ParameterListener.h"

namespace thl {

// ── Anonymous helpers ───────────────────────────────────────────────────────

namespace {
// Non-throwing string-to-double conversion.
// Returns 0.0 for unparseable input, handles bool-like strings.
double parse_string_as_double(const std::string& value) {
    if (value == "true" || value == "1" || value == "yes" || value == "on") { return 1.0; }
    if (value == "false" || value == "0" || value == "no" || value == "off") { return 0.0; }
    const char* const end_init = value.c_str();
    char const* end = nullptr;
    const double result = std::strtod(end_init, const_cast<char**>(&end));
    if (end == end_init) { return 0.0; }  // no conversion performed
    return result;
}
}  // namespace

// ── Constructor & thread registration ───────────────────────────────────────

State::State(size_t max_string_size, size_t max_levels)
    : m_max_string_size(max_string_size)
    , m_max_levels(max_levels)
    , StateGroup(nullptr, nullptr, "")
    , m_string_index_rcu(StringIndexMap{})
    , m_id_index_rcu(IdIndexMap{}) {
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
    m_string_index_rcu.register_reader_thread();
    m_id_index_rcu.register_reader_thread();
    m_groups_rcu.register_reader_thread();
    m_listeners_rcu.register_reader_thread();
}

void State::reserve_temporary_string_buffers() {
    if (m_temp_buffer_0().capacity() < m_max_string_size) {
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

// ── Private helpers: record lookup ──────────────────────────────────────────

ParameterRecord* State::get_record(std::string_view key) const {
    ParameterRecord* record = nullptr;
    m_string_index_rcu.read([&](const StringIndexMap& idx) {
        auto it = idx.find(key);
        if (it != idx.end()) { record = it->second; }
    });
    return record;
}

ParameterRecord* State::get_record_by_id(uint32_t id) const {
    ParameterRecord* record = nullptr;
    m_id_index_rcu.read([&](const IdIndexMap& idx) {
        auto it = idx.find(id);
        if (it == idx.end()) { throw StateKeyNotFoundException("id:" + std::to_string(id)); }
        record = it->second;
    });
    return record;
}

// ── Private helpers: read / write / handle / notify ─────────────────────────

template <typename T>
T State::read_value(ParameterRecord* record, bool allow_blocking) const TANH_NONBLOCKING_FUNCTION {
    if constexpr (std::is_same_v<T, std::string>) {
        if (!allow_blocking) { throw BlockingException(std::string(record->m_key)); }
    }

    ParameterType const param_type = record->m_def.m_type;

    if constexpr (std::is_same_v<T, std::string>) {
        TANH_NONBLOCKING_SCOPED_DISABLER
        switch (param_type) {
            case ParameterType::String: {
                std::scoped_lock const lock(m_storage_mutex);
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
            case ParameterType::String:
                return static_cast<T>(
                    record->m_cache.m_atomic_double.load(std::memory_order_relaxed));
            default: return T{};
        }
    }
}

template <typename T>
void State::write_value(ParameterRecord* record, const T& value) {
    static_assert(std::is_same_v<T, double> || std::is_same_v<T, float> || std::is_same_v<T, int> ||
                      std::is_same_v<T, bool> || std::is_same_v<T, std::string>,
                  "Unsupported type for write_value");

    if constexpr (std::is_same_v<T, std::string>) {
        const double d_value = parse_string_as_double(value);

        if (record->m_def.m_type == ParameterType::String) {
            {
                std::scoped_lock const lock(m_storage_mutex);
                record->m_string_value = value;
            }
            record->m_cache.m_atomic_double.store(d_value, std::memory_order_relaxed);
        } else {
            switch (record->m_def.m_type) {
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
                    record->m_cache.m_atomic_bool.store(d_value != 0.0, std::memory_order_relaxed);
                    break;
                default: break;
            }
        }
    } else {
        switch (record->m_def.m_type) {
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
                    record->m_cache.m_atomic_bool.store(value != T{0}, std::memory_order_relaxed);
                }
                break;
            case ParameterType::String:
                record->m_cache.m_atomic_double.store(static_cast<double>(value),
                                                      std::memory_order_relaxed);
                break;
            default: break;
        }
    }
}

template <typename T>
ParameterHandle<T> State::make_handle(ParameterRecord* record) const {
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
    if (record->m_def.m_type != k_expected_type) {
        throw ParameterTypeMismatchException(k_expected_type, record->m_def.m_type);
    }

    return ParameterHandle<T>(record);
}

void State::notify_after_write(ParameterRecord* record, ParameterListener* source) {
    // Copy key before notifying (key may point into temp buffer
    // which re-entrant set() calls from listeners would overwrite)
    std::string const key_copy(record->m_key);
    Parameter const param_obj(this, record);

    auto [group, param_name] = resolve_path(key_copy);

    if (group) {
        group->notify_listeners(param_obj, source);
    } else {
        const_cast<State*>(this)->notify_listeners(param_obj, source);
    }
}

// ── Parameter creation ──────────────────────────────────────────────────────

void State::create_in_root(std::string_view key, ParameterDefinition def) {
    // Check if parameter already exists
    if (get_record(key)) { throw ParameterAlreadyExistsException(key); }

    // Auto-assign ID if the user didn't provide one.
    // If an explicit ID is provided, advance the counter past it to avoid future collisions.
    if (def.m_id == UINT32_MAX) {
        def.m_id = m_next_auto_id++;
    } else if (def.m_id >= m_next_auto_id) {
        m_next_auto_id = def.m_id + 1;
    }

    ParameterRecord* record = nullptr;
    {
        std::scoped_lock const lock(m_storage_mutex);
        auto new_record = std::make_unique<ParameterRecord>(std::move(def));

        // Set initial value from the definition's default_value
        switch (new_record->m_def.m_type) {
            case ParameterType::Double:
                new_record->m_cache.m_atomic_double.store(
                    static_cast<double>(new_record->m_def.m_default_value),
                    std::memory_order_relaxed);
                break;
            case ParameterType::Float:
                new_record->m_cache.m_atomic_float.store(new_record->m_def.m_default_value,
                                                         std::memory_order_relaxed);
                break;
            case ParameterType::Int:
                new_record->m_cache.m_atomic_int.store(
                    static_cast<int>(new_record->m_def.m_default_value),
                    std::memory_order_relaxed);
                break;
            case ParameterType::Bool:
                new_record->m_cache.m_atomic_bool.store(new_record->m_def.m_default_value != 0.0f,
                                                        std::memory_order_relaxed);
                break;
            case ParameterType::String:
                // String default: store 0.0 in atomic_double
                new_record->m_cache.m_atomic_double.store(0.0, std::memory_order_relaxed);
                break;
            default: break;
        }

        record = new_record.get();
        auto [it, inserted] = m_storage.emplace(std::string(key), std::move(new_record));
        // Set m_key to point into the map node's key (stable for lifetime of entry)
        record->m_key = it->first;
    }

    // Publish the new entry in the lock-free string index
    m_string_index_rcu.update([&](StringIndexMap& idx) { idx[std::string(key)] = record; });

    // Index by ID
    m_id_index_rcu.update([&](IdIndexMap& idx) {
        auto [id_it, id_inserted] = idx.emplace(record->m_def.m_id, record);
        if (!id_inserted) { throw DuplicateParameterIdException(record->m_def.m_id, key); }
    });

    // Notify listeners — copy key before notifying (re-entrant calls may overwrite temp buffers)
    std::string const key_copy(key);
    Parameter const param_obj(this, record);

    auto [group, param_name] = resolve_path(key_copy);
    if (group) {
        group->notify_listeners(param_obj, nullptr);
    } else {
        notify_listeners(param_obj, nullptr);
    }
}

template <typename T>
void State::create_in_root(std::string_view key, const T& initial_value) {
    static_assert(std::is_same_v<T, double> || std::is_same_v<T, float> || std::is_same_v<T, int> ||
                      std::is_same_v<T, bool> || std::is_same_v<T, std::string>,
                  "Unsupported type for create_in_root");

    ParameterDefinition def;
    if constexpr (std::is_same_v<T, double>) {
        def.m_type = ParameterType::Double;
        def.m_default_value = static_cast<float>(initial_value);
    } else if constexpr (std::is_same_v<T, float>) {
        def.m_type = ParameterType::Float;
        def.m_default_value = initial_value;
    } else if constexpr (std::is_same_v<T, int>) {
        def.m_type = ParameterType::Int;
        def.m_default_value = static_cast<float>(initial_value);
    } else if constexpr (std::is_same_v<T, bool>) {
        def.m_type = ParameterType::Bool;
        def.m_default_value = initial_value ? 1.0f : 0.0f;
        def.m_range = Range::boolean();
    } else if constexpr (std::is_same_v<T, std::string>) {
        def.m_type = ParameterType::String;
    }

    // For value-created params, clear default flags (no name = not a plugin param)
    def.m_flags = 0;

    create_in_root(key, std::move(def));

    // m_default_value is float — directly store the full-precision initial value
    // in the atomic cache to avoid truncation for doubles and large ints.
    auto* record = get_record(key);
    if (record) {
        if constexpr (std::is_same_v<T, double>) {
            record->m_cache.m_atomic_double.store(initial_value, std::memory_order_relaxed);
        } else if constexpr (std::is_same_v<T, int>) {
            record->m_cache.m_atomic_int.store(initial_value, std::memory_order_relaxed);
        } else if constexpr (std::is_same_v<T, std::string>) {
            std::scoped_lock const lock(m_storage_mutex);
            record->m_string_value = initial_value;
            const double d_value = parse_string_as_double(initial_value);
            record->m_cache.m_atomic_double.store(d_value, std::memory_order_relaxed);
        }
    }
}

void State::create_in_root(std::string_view key, const char* value) {
    create_in_root(key, std::string(value));
}

// ── Parameter update ────────────────────────────────────────────────────────

template <typename T>
void State::set_in_root(std::string_view key, const T& value, ParameterListener* source) {
    auto* record = get_record(key);
    if (!record) { throw StateKeyNotFoundException(key); }
    write_value(record, value);
    notify_after_write(record, source);
}

void State::set_in_root(std::string_view key, const char* value, ParameterListener* source) {
    set_in_root(key, std::string(value), source);
}

// ── Parameter access ────────────────────────────────────────────────────────

template <typename T>
T State::get_from_root(std::string_view key, bool allow_blocking) const TANH_NONBLOCKING_FUNCTION {
    auto* record = get_record(key);
    if (!record) { throw StateKeyNotFoundException(key); }
    return read_value<T>(record, allow_blocking);
}

Parameter State::get_parameter_from_root(std::string_view key) const {
    auto* record = get_record(key);
    if (!record) { throw StateKeyNotFoundException(key); }
    return {this, record};
}

ParameterType State::get_type_from_root(std::string_view key) const TANH_NONBLOCKING_FUNCTION {
    auto* record = get_record(key);
    if (!record) { throw StateKeyNotFoundException(key); }
    return record->m_def.m_type;
}

template <typename T>
ParameterHandle<T> State::get_handle_from_root(std::string_view key) const {
    auto* record = get_record(key);
    if (!record) { throw StateKeyNotFoundException(key); }
    return make_handle<T>(record);
}

bool State::is_modulatable(std::string_view key) const TANH_NONBLOCKING_FUNCTION {
    auto* record = get_record(key);
    if (!record) { return false; }
    return (record->m_def.m_flags & ParameterFlags::k_modulatable) != 0;
}

// ── ID-based access ─────────────────────────────────────────────────────────

template <typename T>
ParameterHandle<T> State::get_handle_by_id(uint32_t id) const {
    return make_handle<T>(get_record_by_id(id));
}

Parameter State::get_parameter_by_id(uint32_t id) const {
    return {this, get_record_by_id(id)};
}

template <typename T>
T State::get_by_id(uint32_t id, bool allow_blocking) const TANH_NONBLOCKING_FUNCTION {
    return read_value<T>(get_record_by_id(id), allow_blocking);
}

template <typename T>
void State::set_by_id(uint32_t id, const T& value, ParameterListener* source) {
    auto* record = get_record_by_id(id);
    write_value(record, value);
    notify_after_write(record, source);
}

// ── Gesture ─────────────────────────────────────────────────────────────────

void State::set_gesture_from_root(std::string_view key, bool gesture) {
    auto* record = get_record(key);
    if (!record) { return; }

    record->m_in_gesture.store(gesture, std::memory_order_relaxed);

    // Dispatch gesture callbacks through the listener chain
    Parameter const param_obj(this, record);
    std::string const key_copy(key);
    auto [group, param_name] = resolve_path(key_copy);

    auto dispatch = [&](const StateGroup* g) {
        g->m_listeners_rcu.read([&](const StateGroup::ListenerData& data) {
            for (auto* listener : data.m_object_listeners) {
                if (gesture) {
                    listener->on_gesture_start(param_obj);
                } else {
                    listener->on_gesture_end(param_obj);
                }
            }
        });
    };

    if (group) {
        // Walk up from the resolved group to root
        const StateGroup* g = group;
        while (g) {
            dispatch(g);
            g = g->m_parent;
        }
    } else {
        dispatch(this);
    }
}

// ── JSON update ─────────────────────────────────────────────────────────────

void State::update_from_json(const nlohmann::json& json_data, ParameterListener* source) {
    ensure_thread_registered();

    auto check_parameter_exists = [this](std::string_view key) {
        bool const exists = m_string_index_rcu.read(
            [&](const StringIndexMap& idx) { return idx.find(key) != idx.end(); });
        if (!exists) { throw StateKeyNotFoundException(key); }
    };

    std::function<void(const nlohmann::json&, std::string_view)> update_parameters;

    update_parameters = [this, &check_parameter_exists, &update_parameters, &source](
                            const nlohmann::json& json,
                            std::string_view prefix) {
        for (auto it = json.begin(); it != json.end(); ++it) {
            const std::string& key = it.key();
            std::string path;
            if (prefix.empty()) {
                path = key;
            } else {
                m_root_state->m_temp_buffer_0().clear();
                detail::join_path(prefix, key, m_root_state->m_temp_buffer_0());
                path = m_root_state->m_temp_buffer_0();
            }

            if (it.value().is_object()) {
                update_parameters(it.value(), path);
            } else {
                check_parameter_exists(path);

                if (it.value().is_number_float()) {
                    set(path, it.value().get<double>(), source);
                } else if (it.value().is_number_integer()) {
                    set(path, it.value().get<int>(), source);
                } else if (it.value().is_boolean()) {
                    set(path, it.value().get<bool>(), source);
                } else if (it.value().is_string()) {
                    set(path, it.value().get<std::string>(), source);
                } else if (it.value().is_null()) {
                    thl::Logger::logf(thl::Logger::LogLevel::Warning,
                                      "thl.state.state",
                                      "Ignoring null value for key '%s'",
                                      path.c_str());
                }
            }
        }
    };

    try {
        update_parameters(json_data, "");
    } catch (const StateKeyNotFoundException& e) { throw; } catch (const std::exception& e) {
        thl::Logger::logf(thl::Logger::LogLevel::Error,
                          "thl.state.state",
                          "Error updating state from JSON: %s",
                          e.what());
        throw;
    }
}

// ── State dump ──────────────────────────────────────────────────────────────

std::string State::get_state_dump(bool include_definitions) const {
    return get_group_state_dump("", include_definitions);
}

std::string State::get_group_state_dump(std::string_view group_prefix,
                                        bool include_definitions) const {
    nlohmann::json root = nlohmann::json::array();

    std::scoped_lock const lock(m_storage_mutex);
    for (const auto& [key, record] : m_storage) {
        if (!group_prefix.empty() &&
            (key.size() < group_prefix.size() || !key.starts_with(group_prefix))) {
            continue;
        }

        nlohmann::json param_obj = nlohmann::json::object();
        param_obj["key"] = key;

        switch (record->m_def.m_type) {
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

        // Include definition if requested and the parameter has a named definition
        // (parameters created with just a value have empty m_name)
        if (include_definitions && !record->m_def.m_name.empty()) {
            const auto& def = record->m_def;
            nlohmann::json def_obj = nlohmann::json::object();

            def_obj["name"] = def.m_name;
            def_obj["type"] = [&]() {
                switch (def.m_type) {
                    case ParameterType::Float: return "float";
                    case ParameterType::Int: return def.m_choices.empty() ? "int" : "choice";
                    case ParameterType::Bool: return "bool";
                    default: return "unknown";
                }
            }();

            def_obj["min"] = def.m_range.m_min;
            def_obj["max"] = def.m_range.m_max;
            def_obj["step"] = def.m_range.m_step;
            def_obj["skew"] = def.m_range.m_skew;

            def_obj["default_value"] = def.m_default_value;
            def_obj["decimal_places"] = def.m_decimal_places;
            def_obj["automation"] = def.is_automatable();
            def_obj["modulation"] = def.is_modulatable();

            def_obj["slider_polarity"] = [&]() {
                switch (def.m_polarity) {
                    case SliderPolarity::Unipolar: return "unipolar";
                    case SliderPolarity::Bipolar: return "bipolar";
                    default: return "unipolar";
                }
            }();

            if (!def.m_choices.empty()) { def_obj["data"] = def.m_choices; }

            param_obj["definition"] = def_obj;
        }

        root.push_back(param_obj);
    }

    return root.dump();
}

// ── State management ────────────────────────────────────────────────────────

void State::clear() {
    m_string_index_rcu.update([](StringIndexMap& idx) { idx.clear(); });
    m_id_index_rcu.update([](IdIndexMap& idx) { idx.clear(); });

    {
        std::scoped_lock const lock(m_storage_mutex);
        m_storage.clear();
    }

    m_next_auto_id = 0;
    StateGroup::clear_groups();
}

bool State::is_empty() const TANH_NONBLOCKING_FUNCTION {
    bool const parameters_empty =
        m_string_index_rcu.read([](const StringIndexMap& idx) { return idx.empty(); });
    if (!parameters_empty) { return false; }
    return StateGroup::is_empty();
}

// ── Template instantiations ─────────────────────────────────────────────────

template TANH_API void State::create_in_root(std::string_view key, const double& value);
template TANH_API void State::create_in_root(std::string_view key, const float& value);
template TANH_API void State::create_in_root(std::string_view key, const int& value);
template TANH_API void State::create_in_root(std::string_view key, const bool& value);
template TANH_API void State::create_in_root(std::string_view key, const std::string& value);

template TANH_API void State::set_in_root(std::string_view key,
                                          const double& value,
                                          ParameterListener* source);
template TANH_API void State::set_in_root(std::string_view key,
                                          const float& value,
                                          ParameterListener* source);
template TANH_API void State::set_in_root(std::string_view key,
                                          const int& value,
                                          ParameterListener* source);
template TANH_API void State::set_in_root(std::string_view key,
                                          const bool& value,
                                          ParameterListener* source);
template TANH_API void State::set_in_root(std::string_view key,
                                          const std::string& value,
                                          ParameterListener* source);

template TANH_API double State::get_from_root(std::string_view key,
                                              bool allow_blocking) const TANH_NONBLOCKING_FUNCTION;
template TANH_API float State::get_from_root(std::string_view key,
                                             bool allow_blocking) const TANH_NONBLOCKING_FUNCTION;
template TANH_API int State::get_from_root(std::string_view key,
                                           bool allow_blocking) const TANH_NONBLOCKING_FUNCTION;
template TANH_API bool State::get_from_root(std::string_view key,
                                            bool allow_blocking) const TANH_NONBLOCKING_FUNCTION;
template TANH_API std::string State::get_from_root(std::string_view key, bool allow_blocking) const
    TANH_NONBLOCKING_FUNCTION;

template TANH_API ParameterHandle<double> State::get_handle_from_root(std::string_view key) const;
template TANH_API ParameterHandle<float> State::get_handle_from_root(std::string_view key) const;
template TANH_API ParameterHandle<int> State::get_handle_from_root(std::string_view key) const;
template TANH_API ParameterHandle<bool> State::get_handle_from_root(std::string_view key) const;

template TANH_API ParameterHandle<double> State::get_handle_by_id(uint32_t id) const;
template TANH_API ParameterHandle<float> State::get_handle_by_id(uint32_t id) const;
template TANH_API ParameterHandle<int> State::get_handle_by_id(uint32_t id) const;
template TANH_API ParameterHandle<bool> State::get_handle_by_id(uint32_t id) const;

template TANH_API double State::get_by_id(uint32_t id,
                                          bool allow_blocking) const TANH_NONBLOCKING_FUNCTION;
template TANH_API float State::get_by_id(uint32_t id,
                                         bool allow_blocking) const TANH_NONBLOCKING_FUNCTION;
template TANH_API int State::get_by_id(uint32_t id,
                                       bool allow_blocking) const TANH_NONBLOCKING_FUNCTION;
template TANH_API bool State::get_by_id(uint32_t id,
                                        bool allow_blocking) const TANH_NONBLOCKING_FUNCTION;
template TANH_API std::string State::get_by_id(uint32_t id,
                                               bool allow_blocking) const TANH_NONBLOCKING_FUNCTION;

template TANH_API void State::set_by_id(uint32_t id,
                                        const double& value,
                                        ParameterListener* source);
template TANH_API void State::set_by_id(uint32_t id, const float& value, ParameterListener* source);
template TANH_API void State::set_by_id(uint32_t id, const int& value, ParameterListener* source);
template TANH_API void State::set_by_id(uint32_t id, const bool& value, ParameterListener* source);
template TANH_API void State::set_by_id(uint32_t id,
                                        const std::string& value,
                                        ParameterListener* source);

}  // namespace thl
