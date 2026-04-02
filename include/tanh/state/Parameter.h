#pragma once

#include <atomic>
#include <memory>
#include <optional>
#include <string>

#include "ParameterDefinitions.h"
#include "path_helpers.h"
#include "tanh/utils/RealtimeSanitizer.h"

namespace thl {

class State;
class StateGroup;
class ParameterListener;

/**
 * @brief Atomic cache entry for real-time safe per-sample parameter access.
 *
 * Each parameter gets one AtomicCacheEntry, embedded in the owning
 * ParameterRecord. This is the single source of truth for numeric values.
 * ParameterHandle<T> holds a raw pointer to this entry.
 * The entry's address is stable (std::map pointer stability) until
 * State::clear() or State destruction.
 *
 * Only the native-type atomic (matching ParameterRecord::type) is written
 * on set. Reads convert on the fly from the native atomic.
 *
 * @note Non-copyable, non-movable — always accessed via pointer.
 */
struct AtomicCacheEntry {
    std::atomic<double> m_atomic_double{0.0};
    std::atomic<float> m_atomic_float{0.0f};
    std::atomic<int> m_atomic_int{0};
    std::atomic<bool> m_atomic_bool{false};

    AtomicCacheEntry() = default;
    AtomicCacheEntry(const AtomicCacheEntry&) = delete;
    AtomicCacheEntry& operator=(const AtomicCacheEntry&) = delete;
};

/**
 * @brief Lightweight handle for real-time safe per-sample parameter access.
 *
 * Obtained via State::get_handle<T>(). Provides ~1–5 ns atomic load/store,
 * bypassing the RCU path entirely. Only supports numeric types (double, float,
 * int, bool).
 *
 * @section lifetime Handle Lifetime
 *
 * A handle is valid as long as the owning State is alive and State::clear()
 * has not been called. After clear() or State destruction, using a handle is
 * undefined behaviour.
 *
 * @section consistency Consistency
 *
 * The atomic cache is the single source of truth for numeric values.
 * Both State::set_in_root() and ParameterHandle::store() write to the
 * same native-type atomic, so the state always stays in sync.
 *
 * - set_in_root() additionally fires listener notifications.
 * - ParameterHandle::store() is silent (no notifications, no RCU update).
 *
 * Only native-type handles are supported: get_handle<T>() requires T to
 * match the parameter's stored type.
 *
 * @tparam T Numeric type: double, float, int, or bool
 *
 * @note **REAL-TIME SAFE** — load() and store() are wait-free atomic
 *       operations with relaxed memory ordering.
 */
template <typename T>
class ParameterHandle {
    static_assert(std::is_same_v<T, double> || std::is_same_v<T, float> || std::is_same_v<T, int> ||
                      std::is_same_v<T, bool>,
                  "ParameterHandle only supports numeric types (double, float, int, bool)");

public:
    ParameterHandle() : m_entry(nullptr) {}

    T load() const TANH_NONBLOCKING_FUNCTION {
        if constexpr (std::is_same_v<T, double>) {
            return m_entry->m_atomic_double.load(std::memory_order_relaxed);
        } else if constexpr (std::is_same_v<T, float>) {
            return m_entry->m_atomic_float.load(std::memory_order_relaxed);
        } else if constexpr (std::is_same_v<T, int>) {
            return m_entry->m_atomic_int.load(std::memory_order_relaxed);
        } else if constexpr (std::is_same_v<T, bool>) {
            return m_entry->m_atomic_bool.load(std::memory_order_relaxed);
        }
    }

    void store(T value) TANH_NONBLOCKING_FUNCTION {
        if constexpr (std::is_same_v<T, double>) {
            m_entry->m_atomic_double.store(value, std::memory_order_relaxed);
        } else if constexpr (std::is_same_v<T, float>) {
            m_entry->m_atomic_float.store(value, std::memory_order_relaxed);
        } else if constexpr (std::is_same_v<T, int>) {
            m_entry->m_atomic_int.store(value, std::memory_order_relaxed);
        } else if constexpr (std::is_same_v<T, bool>) {
            m_entry->m_atomic_bool.store(value, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] bool is_valid() const TANH_NONBLOCKING_FUNCTION { return m_entry != nullptr; }

private:
    friend class State;
    friend class Parameter;
    explicit ParameterHandle(AtomicCacheEntry* entry) : m_entry(entry) {}
    AtomicCacheEntry* m_entry;
};

// Parameter type enum
enum class ParameterType { Double, Float, Int, Bool, String, Unknown };

// different strategies for notification
enum class NotifyStrategies { All, None, Others, Self };

/**
 * @brief Consolidated per-parameter storage.
 *
 * Each parameter gets one heap-allocated ParameterRecord, owned by
 * State::m_storage via unique_ptr.  The record holds everything about the
 * parameter: type, atomic cache, gesture metadata, string value, and
 * definition.  A thin RCU index (State::m_index_rcu) maps keys to raw
 * pointers into this storage for lock-free lookups.
 *
 * Because AtomicCacheEntry is non-copyable/non-movable the record itself
 * is non-copyable/non-movable — always accessed via pointer.
 */

/// @brief Per-parameter metadata (extensible for future flags).
struct ParameterMetadata {
    /// True while the UI is actively dragging / gesturing this parameter.
    /// Listeners that return false from receives_during_gesture() are
    /// skipped while this flag is set.
    std::atomic<bool> m_in_gesture{false};
};

struct ParameterRecord {
    ParameterType m_type = ParameterType::Double;
    AtomicCacheEntry m_cache;
    ParameterMetadata m_metadata;
    std::string m_string_value;
    std::optional<ParameterDefinition> m_definition;

    ParameterRecord() = default;
    ParameterRecord(const ParameterRecord&) = delete;
    ParameterRecord& operator=(const ParameterRecord&) = delete;
};

/**
 * @class Parameter
 * @brief Object-oriented wrapper for parameter access with type conversion
 * support.
 *
 * Parameter provides a convenient interface for accessing parameter values with
 * automatic type conversion. It wraps a reference to the underlying State and
 * parameter key.
 *
 * @section rt_safety Real-Time Safety
 *
 * - Type checking methods (`is_double()`, `is_float()`, etc.) are **real-time
 * safe**
 * - `to<T>()` for numeric types (double, float, int, bool) is **real-time
 * safe**
 * - `to<std::string>()` requires `allow_blocking=true` as it may allocate
 * memory
 *
 * @see State for the root state container
 * @see StateGroup for hierarchical parameter organization
 */
class Parameter {
public:
    /**
     * @brief Converts the parameter value to the specified type.
     *
     * Provides type conversion with automatic casting between numeric types.
     *
     * @tparam T Target type (double, float, int, bool, or std::string)
     * @param allow_blocking If true, allows blocking operations (required for
     * string type). When false and T is std::string, throws BlockingException.
     *
     * @return The parameter value converted to type T
     *
     * @throws BlockingException if T is std::string and allow_blocking is false
     * @throws StateKeyNotFoundException if the parameter doesn't exist
     *
     * @note **REAL-TIME SAFE** for numeric types (double, float, int, bool)
     * @warning String type requires `allow_blocking=true` as it may allocate
     * memory
     *
     * @par Example:
     * @code
     * Parameter param = state.get_parameter("volume");
     * double vol = param.to<double>();           // Real-time safe
     * std::string str = param.to<std::string>(true);  // Requires
     * allow_blocking=true
     * @endcode
     */
    template <typename T>
    T to(bool allow_blocking = false) const TANH_NONBLOCKING_FUNCTION;

    /**
     * @brief Gets the type of the parameter.
     *
     * @return ParameterType enum indicating the stored type
     *
     * @note **REAL-TIME SAFE**
     */
    ParameterType get_type() const TANH_NONBLOCKING_FUNCTION;

    /**
     * @brief Checks if the parameter is stored as a double.
     * @return true if the parameter type is Double
     * @note **REAL-TIME SAFE**
     */
    bool is_double() const TANH_NONBLOCKING_FUNCTION;

    /**
     * @brief Checks if the parameter is stored as a float.
     * @return true if the parameter type is Float
     * @note **REAL-TIME SAFE**
     */
    bool is_float() const TANH_NONBLOCKING_FUNCTION;

    /**
     * @brief Checks if the parameter is stored as an int.
     * @return true if the parameter type is Int
     * @note **REAL-TIME SAFE**
     */
    bool is_int() const TANH_NONBLOCKING_FUNCTION;

    /**
     * @brief Checks if the parameter is stored as a bool.
     * @return true if the parameter type is Bool
     * @note **REAL-TIME SAFE**
     */
    bool is_bool() const TANH_NONBLOCKING_FUNCTION;

    /**
     * @brief Checks if the parameter is stored as a string.
     * @return true if the parameter type is String
     * @note **REAL-TIME SAFE**
     */
    bool is_string() const TANH_NONBLOCKING_FUNCTION;

    /**
     * @brief Notifies all listeners about this parameter.
     *
     * Triggers parameter change callbacks for registered listeners.
     *
     * @param strategy Notification strategy (all, none, others, self)
     * @param source Source listener to exclude from notifications (optional)
     *
     * @warning NOT real-time safe - invokes listener callbacks
     */
    void notify(NotifyStrategies strategy = NotifyStrategies::All,
                ParameterListener* source = nullptr) const;

    /**
     * @brief Gets the full path for this parameter.
     *
     * @return The dot-separated path string
     *
     * @warning NOT real-time safe - allocates memory for the returned string
     */
    std::string get_path() const;

    /**
     * @brief Gets the parameter definition if one was set.
     *
     * @return A copy of the ParameterDefinition, or std::nullopt if not set
     *
     * @warning NOT real-time safe - copies the definition (may allocate)
     */
    [[nodiscard]] std::optional<ParameterDefinition> get_definition() const;

    /**
     * @brief Returns a typed ParameterHandle for direct atomic load/store.
     *
     * @tparam T Numeric type matching the parameter's native type
     * @return ParameterHandle<T> pointing to the cached AtomicCacheEntry
     *
     * @throws std::invalid_argument if T doesn't match the parameter's native type
     * @throws StateKeyNotFoundException if the parameter doesn't exist
     *
     * @note The returned handle is valid as long as the owning State is alive
     *       and State::clear() has not been called.
     */
    template <typename T>
    [[nodiscard]] ParameterHandle<T> get_handle() const;

private:
    friend class State;
    friend class StateGroup;

    // Private constructors - only State/StateGroup can create Parameters
    Parameter(const State* state, std::string_view key);
    Parameter(const StateGroup* group, std::string_view key, const State* root_state);

    // State reference and parameter key
    const State* m_state;
    std::string m_key;

    // Cached from construction — avoids RCU reads in to<T>() and get_type()
    AtomicCacheEntry* m_cache_ptr = nullptr;
    ParameterType m_type = ParameterType::Unknown;
};

}  // namespace thl
