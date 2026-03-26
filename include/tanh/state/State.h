#pragma once

#include "StateGroup.h"
#include "tanh/utils/RealtimeSanitizer.h"
#include <mutex>
#include <nlohmann/json.hpp>

namespace thl {

/**
 * @class State
 * @brief Root state container providing hierarchical parameter management with
 * real-time safe access.
 *
 * State extends StateGroup to provide the root-level parameter storage using
 * RCU (Read-Copy-Update) for lock-free reads. Parameters are stored in a flat
 * map with dot-separated path keys for efficient lookup while maintaining a
 * hierarchical group structure.
 *
 * @section rt_safety Real-Time Safety
 *
 * Functions marked with `TANH_NONBLOCKING_FUNCTION` are designed to be
 * real-time safe when:
 * - The thread has been registered via ensure_thread_registered()
 * - For numeric types (double, float, int, bool): fully real-time safe
 * - For string types: may allocate if string exceeds SSO buffer size
 *
 * @note Use the `allow_blocking` parameter in getter functions to temporarily
 * disable real-time sanitizer checks when blocking is acceptable.
 *
 * @see StateGroup for group-based parameter access
 * @see RCU for the underlying lock-free read mechanism
 */
class State : public StateGroup {
public:
    /**
     * @brief Constructs a new State object.
     *
     * Initializes RCU for the current thread and pre-allocates string buffers
     * for real-time safe path operations.
     *
     * @param max_string_size Maximum size for pre-allocated string buffers
     * (default: 512)
     * @param max_levels Maximum depth levels for path resolution (default: 10)
     *
     * @warning NOT real-time safe - allocates memory
     */
    State(size_t max_string_size = 512, size_t max_levels = 10);

    /**
     * @brief Ensures the current thread is registered for real-time safe
     * access.
     *
     * Must be called from each thread that will access the state before any
     * parameter operations (The thread that constructs the state is registered
     * automatically!). Registers with all internal RCU structures and
     * pre-allocates string buffers for this State instance.
     *
     * Safe to call multiple times - subsequent calls are no-ops for already
     * registered State instances.
     *
     * @warning NOT real-time safe - should be called during thread
     * initialization, not in the audio processing callback. This will only
     * register the thread for this State instance; and its current child state
     * groups. If the thread will access other State instances, it must register
     * with those separately. if the State will create new child StateGroups
     * after this call on other threads, this thread must reregister again after
     * those groups are created.
     */
    void ensure_thread_registered() override;

    /**
     * @brief Sets a parameter value directly in the root parameter map.
     *
     * Updates existing parameters atomically for numeric types. Creates new
     * parameters if they don't exist (which requires memory allocation).
     *
     * @tparam T Parameter type (double, float, int, bool, or std::string)
     * @param key The parameter key
     * @param value The value to set
     * @param strategy Notification strategy for listeners
     * @param source Source listener to exclude from notifications (optional)
     *
     * @warning NOT real-time safe - may allocate when creating new parameters
     */
    template <typename T>
    void set_in_root(std::string_view key,
                     T value,
                     NotifyStrategies strategy = NotifyStrategies::all,
                     ParameterListener* source = nullptr);

    /**
     * @brief Sets a string parameter from a C-string.
     * @copydetails set_in_root()
     * @warning NOT real-time safe - allocates memory for string conversion
     */
    void set_in_root(std::string_view key,
                     const char* value,
                     NotifyStrategies strategy = NotifyStrategies::all,
                     ParameterListener* source = nullptr);

    /**
     * @brief Updates multiple parameters from a JSON object.
     *
     * Recursively processes the JSON structure and updates corresponding
     * parameters. Parameters must already exist in the state.
     *
     * @param json_data JSON object containing parameter updates
     * @param strategy Notification strategy for listeners
     * @param source Source listener to exclude from notifications
     *
     * @throws StateKeyNotFoundException if a parameter key doesn't exist
     *
     * @warning NOT real-time safe - performs JSON parsing and memory allocation
     */
    void update_from_json(const nlohmann::json& json_data,
                          NotifyStrategies strategy = NotifyStrategies::none,
                          ParameterListener* source = nullptr);

    /**
     * @brief Gets a parameter value directly from the root parameter map.
     *
     * Provides lock-free access to parameter values using RCU.
     *
     * @tparam T Return type (double, float, int, bool, or std::string)
     * @param key The parameter key
     * @param allow_blocking If true, disables real-time sanitizer checks for
     * this call
     *
     * @return The parameter value converted to type T
     *
     * @throws StateKeyNotFoundException if the key doesn't exist
     *
     * @note **REAL-TIME SAFE** for numeric types (double, float, int, bool)
     * @warning String type may allocate if the string exceeds SSO buffer size
     *
     * @par Real-Time Safety
     * This function is marked with `TANH_NONBLOCKING_FUNCTION` and uses RCU for
     * lock-free reads. Numeric type access is fully real-time safe.
     */
    template <typename T>
    T get_from_root(std::string_view key,
                    bool allow_blocking = false) const TANH_NONBLOCKING_FUNCTION;

    /**
     * @brief Gets a Parameter object for the specified key.
     *
     * @param key The parameter key
     * @return Parameter object providing access to the parameter
     *
     * @warning NOT real-time safe - creates a Parameter object
     */
    Parameter get_from_root(std::string_view key) const;

    /**
     * @brief Gets the type of a parameter.
     *
     * @param key The parameter key
     * @return ParameterType enum indicating the parameter's type
     *
     * @throws StateKeyNotFoundException if the key doesn't exist
     *
     * @note **REAL-TIME SAFE** - uses RCU for lock-free access
     */
    ParameterType get_type_from_root(std::string_view key) const TANH_NONBLOCKING_FUNCTION;

    /**
     * @brief Gets a lightweight handle for real-time safe per-sample parameter access.
     *
     * The returned handle provides ~1–5 ns atomic load/store, bypassing the
     * RCU path entirely. Only supports numeric types (double, float, int, bool).
     *
     * @tparam T Numeric type: double, float, int, or bool
     * @param key The parameter key (must already exist)
     *
     * @return ParameterHandle<T> pointing to the parameter's atomic cache entry
     *
     * @throws StateKeyNotFoundException if the key doesn't exist
     *
     * @warning NOT real-time safe — call during setup, then use the handle
     *          on the real-time thread.
     * @warning The handle is valid until State::clear() or State destruction.
     *          Using a handle after either is undefined behaviour.
     *
     * @see ParameterHandle for usage details and consistency guarantees
     */
    template <typename T>
    ParameterHandle<T> get_handle(std::string_view key) const;

    /**
     * @brief Sets the definition for a parameter.
     *
     * Associates a ParameterDefinition with an existing parameter, providing
     * metadata like range, default values, and automation settings.
     *
     * @param key The parameter key (must already exist)
     * @param def The parameter definition
     *
     * @throws StateKeyNotFoundException if the key doesn't exist
     *
     * @warning NOT real-time safe - allocates memory for the definition
     */
    void set_definition_in_root(std::string_view key, const ParameterDefinition& def);

    /**
     * @brief Gets the definition for a parameter.
     *
     * @param key The parameter key
     * @return A copy of the ParameterDefinition, or std::nullopt if not set
     *
     * @throws StateKeyNotFoundException if the key doesn't exist
     *
     * @warning NOT real-time safe - copies the definition (may allocate)
     */
    std::optional<ParameterDefinition> get_definition_from_root(std::string_view key) const;

    /**
     * @brief Sets the gesture state for a parameter.
     *
     * While a parameter is in-gesture, listeners that return false from
     * receives_during_gesture() will be skipped during notifications.
     * This prevents echo-back to the UI during active user drags.
     *
     * @param key The parameter key
     * @param gesture true to begin gesture, false to end
     */
    void set_gesture(std::string_view key, bool gesture);

    /**
     * @brief Clears all parameters and groups from the state.
     *
     * @warning NOT real-time safe - modifies RCU-protected data
     */
    void clear() override;

    /**
     * @brief Checks if the state contains any parameters or groups.
     *
     * @return true if the state is empty, false otherwise
     *
     * @note **REAL-TIME SAFE** - uses RCU for lock-free access
     */
    bool is_empty() const override;

    /**
     * @brief Generates a JSON dump of the entire state.
     *
     * Creates a JSON array containing all parameters with their values and
     * definitions.
     *
     * @param include_definitions Whether to include parameter definitions
     * @return JSON string representation of the state
     *
     * @warning NOT real-time safe - performs JSON serialization and memory
     * allocation
     */
    std::string get_state_dump(bool include_definitions = true) const;

    /**
     * @brief Generates a JSON dump of parameters matching a group prefix.
     *
     * Only includes parameters whose key starts with the given prefix.
     *
     * @param group_prefix Prefix to filter by (e.g., "parameter.engine0")
     * @param include_definitions Whether to include parameter definitions
     * @return JSON string representation of matching parameters
     *
     * @warning NOT real-time safe - performs JSON serialization and memory
     * allocation
     */
    std::string get_group_state_dump(std::string_view group_prefix,
                                     bool include_definitions = true) const;

private:
    friend class Parameter;
    friend class StateGroup;

    /// @brief Thread-local string buffer for path operations (real-time safe
    /// after initialization)
    /// @todo Problem if these are thread_local, how to initialize on audio
    /// thread?
    static inline thread_local std::string m_temp_buffer_0;
    /// @brief Thread-local temporary buffer for path operations (real-time safe
    /// after initialization)
    static inline thread_local std::string m_temp_buffer_1;
    /// @brief Thread-local string buffer for path operations (real-time safe
    /// after initialization)
    static inline thread_local std::string m_temp_buffer_2;
    /// @brief Thread-local string buffer for path operations (real-time safe
    /// after initialization)
    static inline thread_local std::string m_temp_buffer_3;

    /// @brief Owning storage — one heap-allocated ParameterRecord per parameter.
    /// std::map guarantees pointer stability (tree nodes are never moved).
    /// Protected by m_storage_mutex for insertions, erasures, and string/definition access.
    using StorageMap = std::map<std::string, std::unique_ptr<ParameterRecord>, std::less<>>;
    StorageMap m_storage;

    /// @brief Mutex protecting m_storage insertions, erasures, and non-atomic field access
    /// (string_value, definition).  Numeric reads/writes go through the embedded
    /// AtomicCacheEntry and do NOT require this mutex.
    mutable std::mutex m_storage_mutex;

    /// @brief Lock-free index into m_storage.  Each entry is a raw pointer to the
    /// corresponding ParameterRecord.  Copied only when parameters are created or
    /// destroyed (startup / clear), never during normal value writes.
    using IndexMap = std::map<std::string, ParameterRecord*, std::less<>>;
    mutable RCU<IndexMap> m_index_rcu;

    /// @brief Maximum string size for pre-allocated buffers
    size_t m_max_string_size;
    /// @brief Maximum depth levels for path resolution
    size_t m_max_levels;

    /// @brief Registers current thread with this State's RCU structures
    void register_reader_thread();
    /// @brief Reserves thread-local string buffers for path operations
    void reserve_temporary_string_buffers();
};

}  // namespace thl
