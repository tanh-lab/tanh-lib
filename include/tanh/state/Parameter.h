#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

#include "ParameterDefinitions.h"
#include "ParameterListener.h"
#include "tanh/core/Exports.h"
#include "tanh/utils/RealtimeSanitizer.h"

namespace thl {

class State;
class StateGroup;

/**
 * @brief Atomic cache entry for real-time safe per-sample parameter access.
 *
 * Each parameter gets one AtomicCacheEntry, embedded in the owning
 * ParameterRecord. This is the single source of truth for numeric values.
 * ParameterHandle<T> accesses this entry via ParameterRecord*.
 * The entry's address is stable (std::map pointer stability) until
 * State::clear() or State destruction.
 *
 * Only the native-type atomic (matching ParameterRecord::m_def.m_type) is
 * written on set. Reads convert on the fly from the native atomic.
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
 * @brief Consolidated per-parameter storage.
 *
 * Each parameter gets one heap-allocated ParameterRecord, owned by
 * State::m_storage via unique_ptr. The record holds everything about the
 * parameter: immutable definition (type, range, flags, name, etc.),
 * atomic cache for real-time value access, gesture state, and string value.
 *
 * `m_key` is a string_view pointing into the owning std::map node's key.
 * Set once after map insertion, never modified. Zero-cost, no duplication.
 *
 * `m_def` is const — the definition is immutable after construction.
 * RCU publish in create() provides the happens-before guarantee — readers
 * only see the record after `m_key` and `m_def` are fully initialized.
 *
 * Because AtomicCacheEntry is non-copyable/non-movable the record itself
 * is non-copyable/non-movable — always accessed via pointer.
 */
struct ParameterRecord {
    /// Identity (set once after map insertion, never modified)
    std::string_view m_key;

    /// Immutable definition (type, range, flags, name, etc.)
    const ParameterDefinition m_def;

    /// Mutable value (per-sample, lock-free)
    AtomicCacheEntry m_cache;

    /// Mutable runtime state
    std::atomic<bool> m_in_gesture{false};
    std::string m_string_value;  // mutex-protected for String-typed parameters

    explicit ParameterRecord(ParameterDefinition def) : m_def(std::move(def)) {}

    ParameterRecord(const ParameterRecord&) = delete;
    ParameterRecord& operator=(const ParameterRecord&) = delete;
};

/**
 * @brief Lightweight handle for real-time safe per-sample parameter access.
 *
 * Obtained via State::get_handle<T>(). Provides ~1–5 ns atomic load/store,
 * bypassing the RCU path entirely. Only supports numeric types (double, float,
 * int, bool).
 *
 * Stores a ParameterRecord* internally, giving access to both the atomic cache
 * and immutable metadata (definition, range, key, ID, flags) without any
 * additional lookups.
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
    ParameterHandle() = default;

    T load() const TANH_NONBLOCKING_FUNCTION {
        if constexpr (std::is_same_v<T, double>) {
            return m_record->m_cache.m_atomic_double.load(std::memory_order_relaxed);
        } else if constexpr (std::is_same_v<T, float>) {
            return m_record->m_cache.m_atomic_float.load(std::memory_order_relaxed);
        } else if constexpr (std::is_same_v<T, int>) {
            return m_record->m_cache.m_atomic_int.load(std::memory_order_relaxed);
        } else if constexpr (std::is_same_v<T, bool>) {
            return m_record->m_cache.m_atomic_bool.load(std::memory_order_relaxed);
        }
    }

    void store(T value) TANH_NONBLOCKING_FUNCTION {
        if constexpr (std::is_same_v<T, double>) {
            m_record->m_cache.m_atomic_double.store(value, std::memory_order_relaxed);
        } else if constexpr (std::is_same_v<T, float>) {
            m_record->m_cache.m_atomic_float.store(value, std::memory_order_relaxed);
        } else if constexpr (std::is_same_v<T, int>) {
            m_record->m_cache.m_atomic_int.store(value, std::memory_order_relaxed);
        } else if constexpr (std::is_same_v<T, bool>) {
            m_record->m_cache.m_atomic_bool.store(value, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] bool is_valid() const TANH_NONBLOCKING_FUNCTION { return m_record != nullptr; }

    // ── Metadata accessors (RT-safe — all immutable after construction) ──

    [[nodiscard]] const ParameterDefinition& def() const TANH_NONBLOCKING_FUNCTION {
        return m_record->m_def;
    }
    [[nodiscard]] const Range& range() const TANH_NONBLOCKING_FUNCTION {
        return m_record->m_def.m_range;
    }
    [[nodiscard]] std::string_view key() const TANH_NONBLOCKING_FUNCTION { return m_record->m_key; }
    [[nodiscard]] uint32_t id() const TANH_NONBLOCKING_FUNCTION { return m_record->m_def.m_id; }
    [[nodiscard]] ParameterType type() const TANH_NONBLOCKING_FUNCTION {
        return m_record->m_def.m_type;
    }
    [[nodiscard]] uint32_t flags() const TANH_NONBLOCKING_FUNCTION {
        return m_record->m_def.m_flags;
    }

private:
    friend class State;
    friend class Parameter;
    explicit ParameterHandle(ParameterRecord* record) : m_record(record) {}
    ParameterRecord* m_record = nullptr;
};

/**
 * @class Parameter
 * @brief Object-oriented wrapper for parameter access with type conversion
 * support.
 *
 * Parameter holds a ParameterRecord* and a State* — it is a lightweight view
 * over the underlying record. Metadata accessors (def(), range(), key(), id())
 * forward directly to the immutable m_def on the record.
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
class TANH_API Parameter {
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
     *
     * @note **REAL-TIME SAFE** for numeric types (double, float, int, bool)
     * @warning String type requires `allow_blocking=true` as it may allocate
     * memory
     */
    template <typename T>
    T to(bool allow_blocking = false) const TANH_NONBLOCKING_FUNCTION;

    /**
     * @brief Gets the type of the parameter.
     * @return ParameterType enum indicating the stored type
     * @note **REAL-TIME SAFE**
     */
    ParameterType get_type() const TANH_NONBLOCKING_FUNCTION;

    bool is_double() const TANH_NONBLOCKING_FUNCTION;
    bool is_float() const TANH_NONBLOCKING_FUNCTION;
    bool is_int() const TANH_NONBLOCKING_FUNCTION;
    bool is_bool() const TANH_NONBLOCKING_FUNCTION;
    bool is_string() const TANH_NONBLOCKING_FUNCTION;

    /**
     * @brief Notifies all listeners about this parameter.
     *
     * Triggers parameter change callbacks for registered listeners.
     * If source is provided, source->m_strategy controls which listeners
     * receive the notification.
     *
     * @param source Source listener for strategy-based filtering (optional)
     *
     * @warning NOT real-time safe - invokes listener callbacks
     */
    void notify(ParameterListener* source = nullptr) const;

    /**
     * @brief Gets the full path key for this parameter.
     * @return String view into the map node's key (stable for parameter lifetime)
     * @note **REAL-TIME SAFE**
     */
    [[nodiscard]] std::string_view key() const TANH_NONBLOCKING_FUNCTION;

    /**
     * @brief Gets the immutable parameter definition.
     * @return Reference to the const ParameterDefinition
     * @note **REAL-TIME SAFE** — immutable after construction
     */
    [[nodiscard]] const ParameterDefinition& def() const TANH_NONBLOCKING_FUNCTION;

    /**
     * @brief Gets the parameter range.
     * @return Reference to the Range within the definition
     * @note **REAL-TIME SAFE** — immutable after construction
     */
    [[nodiscard]] const Range& range() const TANH_NONBLOCKING_FUNCTION;

    /**
     * @brief Gets the parameter ID (UINT32_MAX if not assigned).
     * @note **REAL-TIME SAFE** — immutable after construction
     */
    [[nodiscard]] uint32_t id() const TANH_NONBLOCKING_FUNCTION;

    /**
     * @brief Returns a typed ParameterHandle for direct atomic load/store.
     *
     * @tparam T Numeric type matching the parameter's native type
     * @return ParameterHandle<T> pointing to the underlying ParameterRecord
     *
     * @throws std::invalid_argument if T doesn't match the parameter's native type
     */
    template <typename T>
    [[nodiscard]] ParameterHandle<T> get_handle() const;

private:
    friend class State;
    friend class StateGroup;

    // Private constructor — only State/StateGroup can create Parameters
    Parameter(const State* state, ParameterRecord* record);

    const State* m_state;
    ParameterRecord* m_record;
};

}  // namespace thl
