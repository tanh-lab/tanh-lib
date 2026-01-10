#pragma once

#include "StateGroup.h"
#include "tanh/utils/RealtimeSanitizer.h"
#include <nlohmann/json.hpp>

namespace thl {

/**
 * @class State
 * @brief Root state container providing hierarchical parameter management with real-time safe access.
 * 
 * State extends StateGroup to provide the root-level parameter storage using RCU (Read-Copy-Update)
 * for lock-free reads. Parameters are stored in a flat map with dot-separated path keys for
 * efficient lookup while maintaining a hierarchical group structure.
 * 
 * @section rt_safety Real-Time Safety
 * 
 * Functions marked with `TANH_NONBLOCKING_FUNCTION` are designed to be real-time safe when:
 * - The thread has been registered via ensure_rcu_initialized()
 * - For numeric types (double, float, int, bool): fully real-time safe
 * - For string types: may allocate if string exceeds SSO buffer size
 * 
 * @note Use the `allow_blocking` parameter in getter functions to temporarily disable 
 *       real-time sanitizer checks when blocking is acceptable.
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
     * @param max_string_size Maximum size for pre-allocated string buffers (default: 512)
     * @param max_levels Maximum depth levels for path resolution (default: 10)
     * 
     * @warning NOT real-time safe - allocates memory
     */
    State(size_t max_string_size = 512, size_t max_levels = 10);
    
    /**
     * @brief Destructor.
     * @warning NOT real-time safe
     */
    ~State();

    /**
     * @brief Ensures RCU is initialized for the current thread.
     * 
     * Must be called from each thread that will access the state before any
     * parameter operations. Initializes thread-local RCU data structures and
     * pre-allocates string buffers.
     * 
     * @warning NOT real-time safe - should be called during thread initialization,
     *          not in the audio processing callback.
     */
    void ensure_rcu_initialized();

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
    template<typename T>
    void set_in_root(std::string_view key, T value, NotifyStrategies strategy = NotifyStrategies::all, ParameterListener* source = nullptr);

    /**
     * @brief Sets a string parameter from a C-string.
     * @copydetails set_in_root()
     * @warning NOT real-time safe - allocates memory for string conversion
     */
    void set_in_root(std::string_view key, const char* value, NotifyStrategies strategy = NotifyStrategies::all, ParameterListener* source = nullptr);

    /**
     * @brief Updates multiple parameters from a JSON object.
     * 
     * Recursively processes the JSON structure and updates corresponding parameters.
     * Parameters must already exist in the state.
     * 
     * @param json_data JSON object containing parameter updates
     * @param strategy Notification strategy for listeners
     * @param source Source listener to exclude from notifications
     * 
     * @throws StateKeyNotFoundException if a parameter key doesn't exist
     * 
     * @warning NOT real-time safe - performs JSON parsing and memory allocation
     */
    void update_from_json(const nlohmann::json& json_data, NotifyStrategies strategy = NotifyStrategies::none, ParameterListener* source = nullptr);

    /**
     * @brief Gets a parameter value directly from the root parameter map.
     * 
     * Provides lock-free access to parameter values using RCU.
     * 
     * @tparam T Return type (double, float, int, bool, or std::string)
     * @param key The parameter key
     * @param allow_blocking If true, disables real-time sanitizer checks for this call
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
    template<typename T>
    T get_from_root(std::string_view key, bool allow_blocking = false) const TANH_NONBLOCKING_FUNCTION;

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
     * @return Pointer to the ParameterDefinition, or nullptr if not set
     * 
     * @throws StateKeyNotFoundException if the key doesn't exist
     * 
     * @note **REAL-TIME SAFE** - uses RCU for lock-free access
     */
    ParameterDefinition* get_definition_from_root(std::string_view key) const TANH_NONBLOCKING_FUNCTION;
    
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
     * Creates a JSON array containing all parameters with their values and definitions.
     * 
     * @return JSON string representation of the state
     * 
     * @warning NOT real-time safe - performs JSON serialization and memory allocation
     */
    std::string get_state_dump() const;

private:
    friend class StateGroup;
    
    /// @brief Thread-local string buffer for path operations (real-time safe after initialization)
    /// @todo Problem if these are thread_local, how to initialize on audio thread?
    static inline thread_local std::string m_path_buffer_1;
    /// @brief Thread-local string buffer for path operations (real-time safe after initialization)
    static inline thread_local std::string m_path_buffer_2; 
    /// @brief Thread-local string buffer for path operations (real-time safe after initialization)
    static inline thread_local std::string m_path_buffer_3;
    /// @brief Thread-local temporary buffer for path operations (real-time safe after initialization)
    static inline thread_local std::string m_temp_buffer;

    /// @brief RCU-protected parameter map for lock-free reads
    using ParameterMap = std::map<std::string, ParameterData, std::less<>>;
    mutable RCU<ParameterMap> m_parameters_rcu;

    /// @brief Maximum string size for pre-allocated buffers
    size_t m_max_string_size;
    /// @brief Maximum depth levels for path resolution
    size_t m_max_levels;

    /// @brief Thread-local flag indicating if RCU has been initialized for this thread
    static inline thread_local bool thread_registered = false;
};

} // namespace thl
