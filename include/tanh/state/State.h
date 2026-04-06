#pragma once

#include "StateGroup.h"
#include "tanh/utils/RealtimeSanitizer.h"
#include <mutex>
#include <nlohmann/json.hpp>
#include <unordered_map>

namespace thl::modulation {
class ModulationMatrix;
}

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
 * Parameters must be explicitly created via create_in_root() before they can
 * be updated with set_in_root(). Each ParameterRecord embeds a const
 * ParameterDefinition that is immutable after creation.
 *
 * @section rt_safety Real-Time Safety
 *
 * Functions marked with `TANH_NONBLOCKING_FUNCTION` are designed to be
 * real-time safe when:
 * - The thread has been registered via ensure_thread_registered()
 * - For numeric types (double, float, int, bool): fully real-time safe
 * - For string types: may allocate if string exceeds SSO buffer size
 *
 * @see StateGroup for group-based parameter access
 * @see RCU for the underlying lock-free read mechanism
 */
class TANH_API State : public StateGroup {
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

    void ensure_thread_registered() override;

    // ── Parameter creation (key-based) ────────────────────────────────────

    /**
     * @brief Creates a parameter with a ParameterDefinition.
     *
     * The definition is moved into the ParameterRecord and becomes immutable.
     * The initial value is set from m_def.m_default_value.
     * If no ID is set on the definition, one is auto-assigned.
     *
     * @param key The parameter key
     * @param def The parameter definition (moved into the record)
     *
     * @throws ParameterAlreadyExistsException if the key already exists
     * @throws DuplicateParameterIdException if an explicit ID collides
     * @warning NOT real-time safe - allocates memory
     */
    void create_in_root(std::string_view key, ParameterDefinition def);

    /**
     * @brief Creates a parameter with an initial value and default definition.
     *
     * Builds a default ParameterDefinition with type inferred from T.
     * An ID is auto-assigned.
     *
     * @tparam T Value type (double, float, int, bool, or std::string)
     * @param key The parameter key
     * @param initial_value The initial value
     *
     * @throws ParameterAlreadyExistsException if the key already exists
     * @warning NOT real-time safe - allocates memory
     */
    template <typename T>
    void create_in_root(std::string_view key, const T& initial_value);

    /**
     * @brief Creates a string parameter from a C-string.
     * @copydetails create_in_root()
     */
    void create_in_root(std::string_view key, const char* value);

    // ── Parameter update (key-based) ──────────────────────────────────────

    /**
     * @brief Sets a parameter value directly in the root parameter map.
     *
     * Updates existing parameters atomically for numeric types.
     * Always notifies listeners. Use ParameterHandle::store() for silent writes.
     *
     * @tparam T Parameter type (double, float, int, bool, or std::string)
     * @param key The parameter key (must already exist)
     * @param value The value to set
     * @param source Source listener for strategy-based notification filtering
     *
     * @throws StateKeyNotFoundException if the key doesn't exist
     *
     * @warning NOT real-time safe for string types
     */
    template <typename T>
    void set_in_root(std::string_view key, const T& value, ParameterListener* source = nullptr);

    /**
     * @brief Sets a string parameter from a C-string.
     * @copydetails set_in_root()
     */
    void set_in_root(std::string_view key, const char* value, ParameterListener* source = nullptr);

    // ── Parameter access (key-based) ──────────────────────────────────────

    /**
     * @brief Gets a parameter value directly from the root parameter map.
     *
     * @tparam T Return type (double, float, int, bool, or std::string)
     * @param key The parameter key
     * @param allow_blocking If true, disables real-time sanitizer checks
     *
     * @return The parameter value converted to type T
     *
     * @throws StateKeyNotFoundException if the key doesn't exist
     *
     * @note **REAL-TIME SAFE** for numeric types (double, float, int, bool)
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
     * @throws StateKeyNotFoundException if the key doesn't exist
     * @warning NOT real-time safe - creates a Parameter object
     */
    Parameter get_parameter_from_root(std::string_view key) const;

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
     * @tparam T Numeric type: double, float, int, or bool
     * @param key The parameter key (must already exist)
     *
     * @return ParameterHandle<T> pointing to the parameter's atomic cache entry
     *
     * @throws StateKeyNotFoundException if the key doesn't exist
     * @throws std::invalid_argument if T doesn't match the parameter's native type
     *
     * @warning NOT real-time safe — call during setup, then use the handle
     *          on the real-time thread.
     */
    template <typename T>
    ParameterHandle<T> get_handle_from_root(std::string_view key) const;

    /**
     * @brief Checks if a parameter has the modulatable flag set.
     *
     * @param key The parameter key
     * @return true if the parameter exists and has kModulatable flag
     *
     * @note **REAL-TIME SAFE** - uses RCU for lock-free access
     */
    bool is_modulatable(std::string_view key) const TANH_NONBLOCKING_FUNCTION;

    // ── ID-based access ──────────────────────────────────────────────────

    /**
     * @brief Gets a typed handle by parameter ID.
     *
     * @tparam T Numeric type matching the parameter's native type
     * @param id The parameter ID (auto-assigned or from ParameterDefinition::m_id)
     * @return ParameterHandle<T> for the matching parameter
     *
     * @throws StateKeyNotFoundException if no parameter has this ID
     * @throws std::invalid_argument if T doesn't match the parameter's type
     * @warning NOT real-time safe — call during setup
     */
    template <typename T>
    ParameterHandle<T> get_handle_by_id(uint32_t id) const;

    /**
     * @brief Gets a Parameter object by parameter ID.
     *
     * @param id The parameter ID (auto-assigned or from ParameterDefinition::m_id)
     * @return Parameter object for the matching parameter
     *
     * @throws StateKeyNotFoundException if no parameter has this ID
     * @warning NOT real-time safe
     */
    Parameter get_parameter_by_id(uint32_t id) const;

    /**
     * @brief Gets a parameter value by ID.
     *
     * @tparam T Return type (double, float, int, bool, or std::string)
     * @param id The parameter ID (auto-assigned or from ParameterDefinition::m_id)
     * @param allow_blocking If true, disables real-time sanitizer checks
     *
     * @return The parameter value converted to type T
     *
     * @throws StateKeyNotFoundException if no parameter has this ID
     *
     * @note **REAL-TIME SAFE** for numeric types (double, float, int, bool)
     */
    template <typename T>
    T get_by_id(uint32_t id, bool allow_blocking = false) const TANH_NONBLOCKING_FUNCTION;

    /**
     * @brief Sets a parameter value by ID.
     *
     * @tparam T Parameter type (double, float, int, bool, or std::string)
     * @param id The parameter ID (auto-assigned or from ParameterDefinition::m_id)
     * @param value The value to set
     * @param source Source listener for strategy-based notification filtering
     *
     * @throws StateKeyNotFoundException if no parameter has this ID
     *
     * @warning NOT real-time safe for string types
     */
    template <typename T>
    void set_by_id(uint32_t id, const T& value, ParameterListener* source = nullptr);

    // ── Gesture ──────────────────────────────────────────────────────────

    /**
     * @brief Sets the gesture state for a parameter.
     *
     * While a parameter is in-gesture, listeners that have
     * m_receives_during_gesture=false will be skipped during notifications.
     * Also dispatches on_gesture_start()/on_gesture_end() callbacks.
     *
     * @param key The parameter key
     * @param gesture true to begin gesture, false to end
     */
    void set_gesture_from_root(std::string_view key, bool gesture);

    // ── Serialization ────────────────────────────────────────────────────

    /**
     * @brief Updates multiple parameters from a JSON object.
     *
     * Recursively processes the JSON structure and updates corresponding
     * parameters. Parameters must already exist in the state.
     *
     * @param json_data JSON object containing parameter updates
     * @param source Source listener for strategy-based notification filtering
     *
     * @throws StateKeyNotFoundException if a parameter key doesn't exist
     *
     * @warning NOT real-time safe - performs JSON parsing and memory allocation
     */
    void update_from_json(const nlohmann::json& json_data, ParameterListener* source = nullptr);

    /**
     * @brief Generates a JSON dump of the entire state.
     *
     * @param include_definitions Whether to include parameter definitions
     * @return JSON string representation of the state
     *
     * @warning NOT real-time safe - performs JSON serialization
     */
    std::string get_state_dump(bool include_definitions = true) const;

    /**
     * @brief Generates a JSON dump of parameters matching a group prefix.
     *
     * @param group_prefix Prefix to filter by (e.g., "parameter.engine0")
     * @param include_definitions Whether to include parameter definitions
     * @return JSON string representation of matching parameters
     *
     * @warning NOT real-time safe - performs JSON serialization
     */
    std::string get_group_state_dump(std::string_view group_prefix,
                                     bool include_definitions = true) const;

    // ── State management ─────────────────────────────────────────────────

    void clear() override;
    bool is_empty() const override;

private:
    friend class Parameter;
    friend class StateGroup;
    friend class modulation::ModulationMatrix;

    static std::string& m_temp_buffer_0() noexcept {
        static thread_local std::string s;
        return s;
    }
    static std::string& m_temp_buffer_1() noexcept {
        static thread_local std::string s;
        return s;
    }
    static std::string& m_temp_buffer_2() noexcept {
        static thread_local std::string s;
        return s;
    }
    static std::string& m_temp_buffer_3() noexcept {
        static thread_local std::string s;
        return s;
    }

    /// @brief Owning storage — one heap-allocated ParameterRecord per parameter.
    /// std::map guarantees pointer stability (tree nodes are never moved).
    using StorageMap = std::map<std::string, std::unique_ptr<ParameterRecord>, std::less<>>;
    StorageMap m_storage;

    /// @brief Mutex protecting m_storage insertions, erasures, and non-atomic field access
    /// (string_value). Numeric reads/writes go through the embedded AtomicCacheEntry
    /// and do NOT require this mutex.
    mutable std::mutex m_storage_mutex;

    /// @brief Lock-free index into m_storage. Each entry is a raw pointer to the
    /// corresponding ParameterRecord. Copied only when parameters are created or
    /// destroyed (startup / clear), never during normal value writes.
    using StringIndexMap = std::map<std::string, ParameterRecord*, std::less<>>;
    mutable RCU<StringIndexMap> m_string_index_rcu;

    /// @brief Lock-free O(1) ID lookup — maps m_def.m_id → ParameterRecord*.
    /// Populated automatically by create_in_root() for every parameter.
    using IdIndexMap = std::unordered_map<uint32_t, ParameterRecord*>;
    mutable RCU<IdIndexMap> m_id_index_rcu;

    size_t m_max_string_size;
    size_t m_max_levels;
    uint32_t m_next_auto_id = 0;

    ParameterRecord* get_record(std::string_view key) const;
    ParameterRecord* get_record_by_id(uint32_t id) const;

    template <typename T>
    T read_value(ParameterRecord* record, bool allow_blocking) const TANH_NONBLOCKING_FUNCTION;

    template <typename T>
    void write_value(ParameterRecord* record, const T& value);

    void notify_after_write(ParameterRecord* record, ParameterListener* source);

    template <typename T>
    ParameterHandle<T> make_handle(ParameterRecord* record) const;

    void register_reader_thread();
    void reserve_temporary_string_buffers();
};

}  // namespace thl
