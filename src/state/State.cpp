#include "tanh/state/State.h"

#include <tanh/core/AbiNamespace.h>
#include <tanh/core/Logger.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "tanh/core/Exports.h"
#include "tanh/parameters/ModulationScope.h"
#include "tanh/parameters/ParameterDefinitions.h"
#include "tanh/state/Exceptions.h"
#include "tanh/state/Parameter.h"
#include "tanh/state/ParameterListener.h"
#include "tanh/state/StateGroup.h"
#include "tanh/state/path_helpers.h"
#include "tanh/utils/RealtimeSanitizer.h"

namespace thl::inline THL_ABI_NAMESPACE {

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

// ── JSON ────────────────────────────────────────────────────────────────────

// A value from JSON in the type it is written with.
using JsonValue = std::variant<double, int, bool, std::string>;

const char* type_name(ParameterType type) {
    switch (type) {
        case ParameterType::Double: return "double";
        case ParameterType::Float: return "float";
        case ParameterType::Int: return "int";
        case ParameterType::Bool: return "bool";
        case ParameterType::String: return "string";
    }
    return "unknown";
}

// A double as an int: truncated, and clamped where it lies outside the range.
int double_to_int(double value) {
    constexpr int k_min = std::numeric_limits<int>::min();
    constexpr int k_max = std::numeric_limits<int>::max();
    if (std::isnan(value)) { return 0; }
    if (value <= static_cast<double>(k_min)) { return k_min; }
    if (value >= static_cast<double>(k_max)) { return k_max; }
    return static_cast<int>(value);
}

// A JSON number as an int: truncated, and clamped where it lies outside the range.
int json_number_to_int(const nlohmann::json& number) {
    constexpr int k_min = std::numeric_limits<int>::min();
    constexpr int k_max = std::numeric_limits<int>::max();
    if (number.is_number_unsigned()) {
        const auto value = number.get<uint64_t>();
        return value > static_cast<uint64_t>(k_max) ? k_max : static_cast<int>(value);
    }
    if (number.is_number_integer()) {
        const auto value = number.get<int64_t>();
        return static_cast<int>(std::clamp<int64_t>(value, k_min, k_max));
    }
    return double_to_int(number.get<double>());
}

// The number a string holds: a number in text, or a word for a bool. Empty when the
// whole string is neither, unlike parse_string_as_double(), which then reads 0.
std::optional<double> string_to_number(const std::string& text) {
    if (text == "true" || text == "yes" || text == "on") { return 1.0; }
    if (text == "false" || text == "no" || text == "off") { return 0.0; }
    const char* const begin = text.c_str();
    char* end = nullptr;
    const double result = std::strtod(begin, &end);
    if (end == begin || end != begin + text.size()) { return std::nullopt; }
    return result;
}

// `value` as a value for a parameter of `type`; empty when it does not fit.
std::optional<JsonValue> json_to_value(const nlohmann::json& value, ParameterType type) {
    if (type == ParameterType::String) {
        if (!value.is_string()) { return std::nullopt; }
        return value.get<std::string>();
    }
    if (value.is_boolean()) { return value.get<bool>(); }

    const bool is_text = value.is_string();
    if (!is_text && !value.is_number()) { return std::nullopt; }
    const std::optional<double> number =
        is_text ? string_to_number(value.get_ref<const std::string&>()) : value.get<double>();
    if (!number) { return std::nullopt; }

    switch (type) {
        case ParameterType::Double:
        case ParameterType::Float: return *number;
        case ParameterType::Int:
            return is_text ? double_to_int(*number) : json_number_to_int(value);
        case ParameterType::Bool: return *number != 0.0;
        case ParameterType::String: break;
    }
    return std::nullopt;
}

// The value of `record`. The caller holds the storage mutex (string values).
nlohmann::json value_to_json(const ParameterRecord& record) {
    switch (record.m_def.m_type) {
        case ParameterType::Double:
            return record.m_cache.m_atomic_double.load(std::memory_order_relaxed);
        case ParameterType::Float:
            return record.m_cache.m_atomic_float.load(std::memory_order_relaxed);
        case ParameterType::Int: return record.m_cache.m_atomic_int.load(std::memory_order_relaxed);
        case ParameterType::Bool:
            return record.m_cache.m_atomic_bool.load(std::memory_order_relaxed);
        case ParameterType::String: return record.m_string_value;
    }
    return nullptr;
}

// Calls `leaf(path, value)` for every value of a nested object that is not an object
// itself, and for every empty object, in the order of the JSON. A key that holds dots is
// a path as it stands, so the flat form is the nested form with one level.
template <typename Leaf>
void for_each_leaf(const nlohmann::json& root, Leaf&& leaf) {
    struct Level {
        const nlohmann::json* m_object;
        nlohmann::json::const_iterator m_next;
        std::string m_path;  // of m_object; unused for the root
        bool m_is_root;
    };
    std::vector<Level> levels;
    levels.push_back({.m_object = &root, .m_next = root.cbegin(), .m_path = {}, .m_is_root = true});
    while (!levels.empty()) {
        Level& level = levels.back();
        if (level.m_next == level.m_object->cend()) {
            levels.pop_back();
            continue;
        }
        const auto member = level.m_next++;
        std::string path = level.m_is_root ? member.key() : level.m_path + '.' + member.key();
        if (member.value().is_object() && !member.value().empty()) {
            const nlohmann::json& child = member.value();
            // Ends `level`: the vector may move.
            levels.push_back({.m_object = &child,
                              .m_next = child.cbegin(),
                              .m_path = std::move(path),
                              .m_is_root = false});
        } else {
            leaf(path, member.value());
        }
    }
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

    // Every write through State is a host change for a ModulationMatrix over
    // it. After the value, with release: a reader that sees the new stamp
    // (acquire) sees the new value.
    record->m_change_stamp.fetch_add(1, std::memory_order_release);
}

template <typename T>
ParameterHandle<T> State::make_handle(ParameterRecord* record) const {
    constexpr ParameterType k_expected_type = parameter_type_of<T>();
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

    // Ensure every parameter has working formatters
    def.ensure_formatters();

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

State::JsonReadReport State::from_json(const nlohmann::json& json_data,
                                       ParameterListener* source,
                                       JsonMode mode) {
    ensure_thread_registered();

    struct Write {
        std::string m_path;
        JsonValue m_value;
    };
    std::vector<Write> writes;
    JsonReadReport report;

    // First every entry is resolved and converted; nothing is written before the last one
    // passed.
    auto skip = [&report](const std::string& path, const char* reason) {
        thl::Logger::logf(thl::Logger::LogLevel::Warning,
                          "thl.state.state",
                          "from_json: skipping '%s': %s",
                          path.c_str(),
                          reason);
        report.m_skipped.push_back(path);
    };

    auto add = [&](const std::string& path, const nlohmann::json& value) {
        const ParameterRecord* const record = get_record(path);
        if (!record) {
            if (mode == JsonMode::Strict) { throw StateKeyNotFoundException(path); }
            skip(path, "not a parameter");
            return;
        }
        if (value.is_null()) {
            skip(path, "null value");
            return;
        }
        std::optional<JsonValue> converted = json_to_value(value, record->m_def.m_type);
        if (!converted) {
            const std::string reason = std::string("a JSON ") + value.type_name() +
                                       " does not fit a " + type_name(record->m_def.m_type) +
                                       " parameter";
            if (mode == JsonMode::Strict) {
                throw StateJsonException(path, "Cannot apply '" + path + "': " + reason);
            }
            skip(path, reason.c_str());
            return;
        }
        writes.push_back({.m_path = path, .m_value = std::move(*converted)});
    };

    if (json_data.is_object()) {
        for_each_leaf(json_data, [&](const std::string& path, const nlohmann::json& value) {
            // An empty object is a group without values, unless its path is a parameter.
            if (value.is_object() && !get_record(path)) { return; }
            add(path, value);
        });
    } else if (json_data.is_array()) {
        static const nlohmann::json k_null;
        for (const nlohmann::json& entry : json_data) {
            const auto key = entry.is_object() ? entry.find("key") : entry.end();
            if (key == entry.end() || !key->is_string()) {
                throw StateJsonException(
                    "",
                    "State JSON: a record of the array has no \"key\" that is a string");
            }
            const auto value = entry.find("value");
            add(key->get_ref<const std::string&>(), value != entry.end() ? *value : k_null);
        }
    } else if (!json_data.is_null()) {
        throw StateJsonException("",
                                 std::string("State JSON: expected an object or an array, got ") +
                                     json_data.type_name());
    }

    // Looked up again: a listener of an earlier write may have removed the parameter.
    for (const Write& write : writes) {
        ParameterRecord* const record = get_record(write.m_path);
        if (!record) {
            skip(write.m_path, "removed by a listener during the call");
            continue;
        }
        std::visit([&](const auto& value) { write_value(record, value); }, write.m_value);
        notify_after_write(record, source);
        ++report.m_applied;
    }
    return report;
}

// ── State dump ──────────────────────────────────────────────────────────────

nlohmann::json State::to_json(bool include_definitions) const {
    return group_to_json("", include_definitions);
}

nlohmann::json State::values_to_json(std::string_view group_prefix) const {
    nlohmann::json root = nlohmann::json::object();

    auto clash = [](const std::string& key) {
        return StateJsonException(key,
                                  "Cannot write '" + key +
                                      "' as a nested object: a parameter lies below another "
                                      "parameter. Use to_json().");
    };

    std::scoped_lock const lock(m_storage_mutex);
    for (const auto& [key, record] : m_storage) {
        if (!detail::in_group(key, group_prefix)) { continue; }

        nlohmann::json* node = &root;
        std::string_view rest = key;
        for (size_t dot = rest.find('.'); dot != std::string_view::npos; dot = rest.find('.')) {
            node = &(*node)[std::string(rest.substr(0, dot))];
            if (node->is_null()) { *node = nlohmann::json::object(); }
            if (!node->is_object()) { throw clash(key); }
            rest.remove_prefix(dot + 1);
        }
        nlohmann::json& leaf = (*node)[std::string(rest)];
        if (leaf.is_object()) { throw clash(key); }
        leaf = value_to_json(*record);
    }

    return root;
}

nlohmann::json State::group_to_json(std::string_view group_prefix, bool include_definitions) const {
    nlohmann::json root = nlohmann::json::array();

    std::scoped_lock const lock(m_storage_mutex);
    for (const auto& [key, record] : m_storage) {
        if (!detail::in_group(key, group_prefix)) { continue; }

        nlohmann::json param_obj = nlohmann::json::object();
        param_obj["key"] = key;
        param_obj["value"] = value_to_json(*record);

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
            if (def.m_range.m_curve.m_type == NormalizationCurve::Type::PowerLaw) {
                def_obj["skew"] = def.m_range.m_curve.m_skew;
            }
            if (def.m_range.m_periodic) { def_obj["periodic"] = true; }

            def_obj["default_value"] = def.m_default_value;
            def_obj["decimal_places"] = def.m_decimal_places;
            def_obj["unit"] = def.m_unit;
            def_obj["automation"] = def.is_automatable();
            def_obj["modulation"] = def.is_modulatable();
            // Omit for Global (default) to keep presets lean; every other
            // scope has a non-empty name by ModulationMatrix::register_scope
            // construction.
            if (def.m_modulation_scope != modulation::k_global_scope) {
                def_obj["modulationScope"] = def.m_modulation_scope.m_name;
            }

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

    return root;
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

}  // namespace thl::inline THL_ABI_NAMESPACE
