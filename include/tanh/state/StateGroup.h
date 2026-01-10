#pragma once

#include <string>
#include <string_view>
#include <map>
#include <memory>
#include <vector>
#include <iostream>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <functional>

#include "tanh/core/threading/RCU.h"
#include "tanh/utils/RealtimeSanitizer.h"

#include "Parameter.h"
#include "path_helpers.h"

namespace thl {

/**
 * @class StateKeyNotFoundException
 * @brief Exception thrown when a parameter key is not found in the state.
 */
class StateKeyNotFoundException : public std::runtime_error {
public:
    /**
     * @brief Constructs the exception with the missing key.
     * @param key The key that was not found
     */
    explicit StateKeyNotFoundException(std::string_view key) 
        : std::runtime_error("Key not found in state: " + std::string(key)), m_key(key) {}
    
    /**
     * @brief Gets the key that was not found.
     * @return Reference to the missing key string
     */
    const std::string& key() const { return m_key; }
    
private:
    std::string m_key;
};

/**
 * @class StateGroupNotFoundException
 * @brief Exception thrown when a state group is not found.
 */
class StateGroupNotFoundException : public std::runtime_error {
public:
    /**
     * @brief Constructs the exception with the missing group name.
     * @param groupName The name of the group that was not found
     */
    explicit StateGroupNotFoundException(std::string_view groupName) 
        : std::runtime_error("State group not found: " + std::string(groupName)), m_groupName(groupName) {}
    
private:
    std::string m_groupName;
};

/**
 * @class BlockingException
 * @brief Exception thrown when attempting a blocking operation without allow_blocking flag.
 * 
 * Certain operations (like string access) may allocate memory, which is not real-time safe.
 * This exception enforces explicit acknowledgment of the blocking nature by requiring
 * allow_blocking=true for such operations.
 */
class BlockingException : public std::runtime_error {
public:
    /**
     * @brief Constructs the exception with the parameter path.
     * @param path The path of the parameter being accessed
     */
    explicit BlockingException(std::string_view path) 
        : std::runtime_error("Blocking operation requires allow_blocking=true: " + std::string(path)), m_path(path) {}
    
    /**
     * @brief Gets the path that was being accessed.
     * @return Reference to the path string
     */
    const std::string& path() const { return m_path; }

private:
    std::string m_path;
};

// Forward declarations
class State;
class StateGroup;
class Parameter;

/**
 * @class ParameterListener
 * @brief Abstract interface for receiving parameter change notifications.
 * 
 * Implement this interface to receive callbacks when parameters are modified.
 * Listeners can be registered with StateGroup::add_listener().
 */
class ParameterListener {
public:
    virtual ~ParameterListener() = default;
    
    /**
     * @brief Called when a parameter value changes.
     * 
     * @param path The full path of the changed parameter
     * @param param The Parameter object with the new value
     * 
     * @warning The real-time safety of this callback depends on what the
     *          implementation does. Avoid allocations and blocking operations
     *          if called from a real-time context.
     */
    virtual void on_parameter_changed(std::string_view path, const Parameter& param) = 0;
};

/**
 * @brief Function-based callback type for parameter changes.
 * @param path The full path of the changed parameter
 * @param param The Parameter object with the new value
 */
using ParameterChangeCallback = std::function<void(std::string_view path, const Parameter& param)>;

/**
 * @class StateGroup
 * @brief Hierarchical container for organizing parameters into logical groups.
 * 
 * StateGroup provides a tree structure for organizing parameters with dot-separated
 * path support (e.g., "synth.oscillator.frequency"). Groups can contain parameters
 * and nested subgroups.
 * 
 * @section rt_safety Real-Time Safety
 * 
 * Functions marked with `TANH_NONBLOCKING_FUNCTION` are designed to be real-time safe:
 * - They use RCU (Read-Copy-Update) for lock-free reads
 * - For numeric types: fully real-time safe
 * - For string types: may allocate if string exceeds SSO buffer size
 * 
 * @note The `allow_blocking` parameter in getter functions allows temporarily
 *       disabling real-time sanitizer checks when blocking is acceptable.
 * 
 * @see State for the root state container
 */
class StateGroup {
public:
    /**
     * @brief Creates a new child group.
     * 
     * Creates a subgroup with the specified name. If the group already exists,
     * returns the existing group.
     * 
     * @param name The name of the group to create
     * @return Pointer to the created or existing group
     * 
     * @warning NOT real-time safe - may allocate memory for new groups
     */
    StateGroup* create_group(std::string_view name);
    
    /**
     * @brief Gets a child group by name.
     * 
     * @param name The name of the group to retrieve
     * @return Pointer to the group, or nullptr if not found
     * 
     * @note **REAL-TIME SAFE** - uses RCU for lock-free access
     */
    StateGroup* get_group(std::string_view name) const TANH_NONBLOCKING_FUNCTION;
    
    /**
     * @brief Checks if a child group exists.
     * 
     * @param name The name of the group to check
     * @return true if the group exists, false otherwise
     * 
     * @note **REAL-TIME SAFE** - uses RCU for lock-free access
     */
    bool has_group(std::string_view name) const TANH_NONBLOCKING_FUNCTION;
    
    /**
     * @brief Gets a Parameter object for the specified path.
     * 
     * Navigates through nested groups using dot-separated paths.
     * 
     * @param path The parameter path (e.g., "oscillator.frequency")
     * @return Parameter object providing access to the parameter
     * 
     * @throws StateKeyNotFoundException if the parameter doesn't exist
     * @throws StateGroupNotFoundException if an intermediate group doesn't exist
     * 
     * @warning NOT real-time safe (BLOCKING) - creates a Parameter object
     */
    Parameter get_parameter(std::string_view path) const;

    /**
     * @brief Gets all parameters in this group and its subgroups.
     * 
     * @return Map of parameter paths to Parameter objects
     * 
     * @warning NOT real-time safe - allocates memory for the returned map
     */
    std::map<std::string, Parameter> get_parameters() const;
    
    /**
     * @brief Sets a parameter value at the specified path.
     * 
     * Creates intermediate groups and the parameter if they don't exist (when create=true).
     * 
     * @tparam T Parameter type (double, float, int, bool, std::string, or ParameterDefinition types)
     * @param path The parameter path (e.g., "oscillator.frequency")
     * @param value The value to set
     * @param strategy Notification strategy for listeners
     * @param source Source listener to exclude from notifications
     * @param create If true, creates missing groups and parameters
     * 
     * @throws StateKeyNotFoundException if create=false and parameter doesn't exist
     * @throws StateGroupNotFoundException if create=false and intermediate group doesn't exist
     * 
     * @warning NOT real-time safe - may allocate memory for new parameters/groups
     */
    template<typename T>
    void set(std::string_view path, T value, NotifyStrategies strategy = NotifyStrategies::all, ParameterListener* source = nullptr, bool create = true);

    /**
     * @brief Sets a string parameter from a C-string.
     * @copydetails set()
     * @warning NOT real-time safe - allocates memory for string conversion
     */
    void set(std::string_view path, const char* value, NotifyStrategies strategy = NotifyStrategies::all, ParameterListener* source = nullptr, bool create = true);

    /**
     * @brief Gets a parameter value from the specified path.
     * 
     * Provides lock-free access using RCU for numeric types.
     * 
     * @tparam T Return type (double, float, int, bool, or std::string)
     * @param path The parameter path (e.g., "oscillator.frequency")
     * @param allow_blocking If true, disables real-time sanitizer checks for this call
     * 
     * @return The parameter value converted to type T
     * 
     * @throws StateKeyNotFoundException if the parameter doesn't exist
     * @throws StateGroupNotFoundException if an intermediate group doesn't exist
     * 
     * @note **REAL-TIME SAFE** for numeric types (double, float, int, bool)
     * @warning String type may allocate if the string exceeds SSO buffer size
     * 
     * @par Real-Time Safety
     * This function is marked with `TANH_NONBLOCKING_FUNCTION`. Uses
     * `TANH_NONBLOCKING_SCOPED_DISABLER` when allow_blocking=true.
     */
    template<typename T>
    T get(std::string_view path, bool allow_blocking = false) const TANH_NONBLOCKING_FUNCTION;
    
    /**
     * @brief Gets the type of a parameter at the specified path.
     * 
     * @param path The parameter path
     * @return ParameterType enum indicating the parameter's type
     * 
     * @throws StateKeyNotFoundException if the parameter doesn't exist
     * 
     * @note **REAL-TIME SAFE** - uses RCU for lock-free access
     */
    ParameterType get_parameter_type(std::string_view path) const TANH_NONBLOCKING_FUNCTION;
    
    /**
     * @brief Adds a listener for parameter changes in this group.
     * 
     * The listener will receive notifications for all parameter changes in this
     * group and its subgroups.
     * 
     * @param listener Pointer to the listener (must remain valid while registered)
     * 
     * @warning NOT real-time safe - modifies listener list
     */
    void add_listener(ParameterListener* listener);
    
    /**
     * @brief Removes a previously added listener.
     * 
     * @param listener Pointer to the listener to remove
     * 
     * @warning NOT real-time safe - modifies listener list
     */
    void remove_listener(ParameterListener* listener);
    
    /**
     * @brief Adds a callback-based listener for parameter changes.
     * 
     * @param callback The callback function to invoke on parameter changes
     * @return Unique ID for the callback (used for removal)
     * 
     * @warning NOT real-time safe - modifies listener list and allocates
     */
    size_t add_callback_listener(ParameterChangeCallback callback);
    
    /**
     * @brief Removes a callback listener by ID.
     * 
     * @param listener_id The ID returned by add_callback_listener()
     * 
     * @warning NOT real-time safe - modifies listener list
     */
    void remove_callback_listener(size_t listener_id);
    
    /**
     * @brief Manually triggers notifications for a parameter.
     * 
     * Useful when you need to force a notification without changing the value.
     * 
     * @param path The parameter path to notify about
     * 
     * @warning NOT real-time safe - invokes listener callbacks
     */
    void notify_parameter_change(std::string_view path);
    
    /**
     * @brief Clears all parameters and subgroups from this group.
     * 
     * @warning NOT real-time safe - modifies RCU-protected data structures
     */
    virtual void clear();
    
    /**
     * @brief Clears all subgroups from this group.
     * 
     * @warning NOT real-time safe - modifies RCU-protected data structures
     */
    void clear_groups();
    
    /**
     * @brief Checks if this group contains any parameters or subgroups.
     * 
     * @return true if the group is empty, false otherwise
     * 
     * @note **REAL-TIME SAFE** - uses RCU for lock-free access
     */
    virtual bool is_empty() const TANH_NONBLOCKING_FUNCTION;
    
    /**
     * @brief Gets the parent group.
     * 
     * @return Pointer to the parent group, or nullptr for root
     * 
     * @note **REAL-TIME SAFE** - simple pointer access
     */
    StateGroup* get_parent() const { return m_parent; }
    
    /**
     * @brief Gets the root State object.
     * 
     * @return Pointer to the root State
     * 
     * @note **REAL-TIME SAFE** - simple pointer access
     */
    State* get_root_state() const { return m_rootState; }
    
    /**
     * @brief Gets the full dot-separated path from the root to this group.
     * 
     * @return String view of the full path
     * 
     * @note **REAL-TIME SAFE** - uses pre-allocated string buffers
     */
    std::string_view get_full_path() const TANH_NONBLOCKING_FUNCTION;
    
    /**
     * @brief Constructs a StateGroup.
     * 
     * @param rootState Pointer to the root State object
     * @param parent Pointer to the parent group (nullptr for root)
     * @param name The name of this group
     * 
     * @note Made public for std::make_unique, but typically groups are created
     *       via StateGroup::create_group()
     * 
     * @warning NOT real-time safe - initialization
     */
    StateGroup(State* rootState, StateGroup* parent = nullptr, std::string_view name = "");
    
private:
    friend class State;
    friend class Parameter;
    
    /**
     * @brief Internal: Sets a parameter with a ParameterDefinition.
     * 
     * Registers the definition and sets the default value.
     * 
     * @param path The parameter path
     * @param def The parameter definition
     * @param strategy Notification strategy
     * @param source Source listener to exclude from notifications
     * @param create If true, creates missing groups and parameters
     * 
     * @warning NOT real-time safe - allocates memory
     */
    void set(std::string_view path, const ParameterDefinition& def, NotifyStrategies strategy = NotifyStrategies::all, ParameterListener* source = nullptr, bool create = true);
    
    /**
     * @brief Notifies all listeners about a parameter change.
     * 
     * Propagates notifications up the group hierarchy to parent groups.
     * 
     * @param path The parameter path
     * @param param The Parameter object
     * @param strategy Notification strategy
     * @param source Source listener to exclude
     * 
     * @note Uses RCU for lock-free listener access, but listener callbacks
     *       may not be real-time safe depending on implementation.
     */
    void notify_listeners(std::string_view path, const Parameter& param, NotifyStrategies strategy = NotifyStrategies::all, ParameterListener* source = nullptr) const;
    
    /**
     * @brief Resolves a dot-separated path to a group and parameter name.
     * 
     * @param path The path to resolve
     * @return Pair of (group pointer, parameter name)
     * 
     * @throws StateGroupNotFoundException if an intermediate group doesn't exist
     * 
     * @warning NOT real-time safe - may throw exceptions
     */
    std::pair<StateGroup*, std::string_view> resolve_path(std::string_view path) const;
    
    /**
     * @brief Resolves a path, creating missing groups as needed.
     * 
     * @param path The path to resolve
     * @return Pair of (group pointer, parameter name)
     * 
     * @warning NOT real-time safe - may allocate memory for new groups
     */
    std::pair<StateGroup*, std::string_view> resolve_path_create(std::string_view path);
    
    /// @brief Pointer to the root State object
    State* m_rootState;
    /// @brief Pointer to the parent group (nullptr for root)
    StateGroup* m_parent;
    /// @brief Name of this group
    std::string m_name;
    
    /**
     * @brief Listener data structure for RCU protection.
     */
    struct ListenerData {
        std::vector<ParameterListener*> object_listeners;  ///< Object-based listeners
        std::map<size_t, ParameterChangeCallback> callback_listeners;  ///< Callback-based listeners
        size_t next_listener_id = 0;  ///< Next available listener ID
        
        /// @brief Default constructor
        ListenerData() = default;
        
        /// @brief Copy constructor for RCU
        ListenerData(const ListenerData& other) 
            : object_listeners(other.object_listeners),
              callback_listeners(other.callback_listeners),
              next_listener_id(other.next_listener_id) {}
    };
    
    /// @brief RCU-protected listeners for lock-free notification dispatch
    mutable RCU<ListenerData> m_listeners_rcu;

    /// @brief RCU-protected groups map for lock-free group access
    using GroupMap = std::map<std::string, std::shared_ptr<StateGroup>>;
    mutable RCU<GroupMap> m_groups_rcu;
};

} // namespace thl
