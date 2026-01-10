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

#ifdef TANH_WITH_RTSAN
#include <sanitizer/rtsan_interface.h>
#endif

#include "tanh/core/threading/RCU.h"
#include "tanh/utils/RealtimeSanitizer.h"

#include "Parameter.h"
#include "path_helpers.h"

namespace thl {

// Custom exception for key not found in state
class StateKeyNotFoundException : public std::runtime_error {
public:
    explicit StateKeyNotFoundException(std::string_view key) 
        : std::runtime_error("Key not found in state: " + std::string(key)), m_key(key) {}
    ~StateKeyNotFoundException() {
        TANH_NONBLOCKING_DISABLE // Disable RT sanitizer during stack unwinding, needed if exception thrown in RT context
    }
    
    const std::string& key() const { return m_key; }
    
private:
    std::string m_key;
};

class StateGroupNotFoundException : public std::runtime_error {
public:
    explicit StateGroupNotFoundException(std::string_view groupName) 
        : std::runtime_error("State group not found: " + std::string(groupName)), m_groupName(groupName) {}
    ~StateGroupNotFoundException() {
        TANH_NONBLOCKING_DISABLE // Disable RT sanitizer during stack unwinding, needed if exception thrown in RT context
    }

private:
    std::string m_groupName;
};

// Forward declarations
class State;
class StateGroup;
class Parameter;

// Parameter listener interface
class ParameterListener {
public:
    virtual ~ParameterListener() = default;
    
    // Called when a parameter value changes
    virtual void on_parameter_changed(std::string_view path, const Parameter& param) = 0;
};

// Alternatively, use function-based approach with std::function
using ParameterChangeCallback = std::function<void(std::string_view path, const Parameter& param)>;

// State group class for hierarchical organization
class StateGroup {
public:
    // Group management
    StateGroup* create_group(std::string_view name);
    StateGroup* get_group(std::string_view name) const;
    bool has_group(std::string_view name) const;
    
    // Parameter access with path support (can navigate through nested groups), !BLOCKING
    Parameter get_parameter(std::string_view path) const;

    // Get all parameters in this group
    std::map<std::string, Parameter> get_parameters() const;
    
    // Parameter management with path support 
    template<typename T>
    void set(std::string_view path, T value, NotifyStrategies strategy = NotifyStrategies::all, ParameterListener* source = nullptr, bool create = true);

    // Special case for const char* to std::string
    void set(std::string_view path, const char* value, NotifyStrategies strategy = NotifyStrategies::all, ParameterListener* source = nullptr, bool create = true);

    // Parameter getters (real-time safe for numeric types)
    template<typename T>
    T get(std::string_view path, bool allow_blocking = false) const;
    
    // Get parameter type with path support
    ParameterType get_parameter_type(std::string_view path) const;
    
    // Listener management
    void add_listener(ParameterListener* listener);
    void remove_listener(ParameterListener* listener);
    
    // Function-based listener management
    size_t add_callback_listener(ParameterChangeCallback callback);
    void remove_callback_listener(size_t listener_id);
    
    // Manually trigger notifications for a parameter
    void notify_parameter_change(std::string_view path);
    
    // State management
    virtual void clear();
    void clear_groups();
    virtual bool is_empty() const;
    
    // Get parent and root info
    StateGroup* get_parent() const { return m_parent; }
    State* get_root_state() const { return m_rootState; }
    std::string_view get_full_path() const;
    
    // Constructor - made public for std::make_unique
    StateGroup(State* rootState, StateGroup* parent = nullptr, std::string_view name = "");
    
private:
    friend class State;
    friend class Parameter;
    
    // Internal: Set with parameter definition (registers definition and sets default value)
    // This is private - users should use typed versions (ParameterFloat, ParameterInt, etc.)
    void set(std::string_view path, const ParameterDefinition& def, NotifyStrategies strategy = NotifyStrategies::all, ParameterListener* source = nullptr, bool create = true);
    
    // Notify all listeners about a parameter change
    void notify_listeners(std::string_view path, const Parameter& param, NotifyStrategies strategy = NotifyStrategies::all, ParameterListener* source = nullptr) const;
    
    // Helper for parameter resolution with paths
    std::pair<StateGroup*, std::string_view> resolve_path(std::string_view path) const;
    
    // Helper for parameter resolution with paths that creates missing groups
    std::pair<StateGroup*, std::string_view> resolve_path_create(std::string_view path);
    
    // Member variables
    State* m_rootState;
    StateGroup* m_parent;
    std::string m_name;
    
    // RCU-protected listeners for lock-free notification
    struct ListenerData {
        std::vector<ParameterListener*> object_listeners;
        std::map<size_t, ParameterChangeCallback> callback_listeners;
        size_t next_listener_id = 0;
        
        // Default constructor
        ListenerData() = default;
        
        // Copy constructor for RCU
        ListenerData(const ListenerData& other) 
            : object_listeners(other.object_listeners),
              callback_listeners(other.callback_listeners),
              next_listener_id(other.next_listener_id) {}
    };
    
    mutable RCU<ListenerData> m_listeners_rcu;

    // RCU-protected groups for lock-free access
    using GroupMap = std::map<std::string, std::shared_ptr<StateGroup>>;
    mutable RCU<GroupMap> m_groups_rcu;
};

} // namespace thl
