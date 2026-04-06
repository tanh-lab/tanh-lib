#pragma once

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "Parameter.h"
#include "ParameterListener.h"
#include "tanh/core/Exports.h"
#include "tanh/core/threading/RCU.h"
#include "tanh/utils/RealtimeSanitizer.h"

namespace thl {

// Forward declarations
class State;
class Parameter;

/**
 * @class StateGroup
 * @brief Hierarchical container for organizing parameters into logical groups.
 *
 * StateGroup provides a tree structure for organizing parameters with
 * dot-separated path support (e.g., "synth.oscillator.frequency"). Groups can
 * contain parameters and nested subgroups.
 */
class TANH_API StateGroup {
public:
    StateGroup(State* root_state, StateGroup* parent = nullptr, std::string_view name = "");
    virtual ~StateGroup();

    // ── Group management ────────────────────────────────────────────────

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
     * @param name The name of the group to retrieve
     * @return Pointer to the group, or nullptr if not found
     * @note **REAL-TIME SAFE** - uses RCU for lock-free access
     */
    StateGroup* get_group(std::string_view name) const TANH_NONBLOCKING_FUNCTION;

    /**
     * @brief Checks if a child group exists.
     * @param name The name of the group to check
     * @return true if the group exists, false otherwise
     * @note **REAL-TIME SAFE** - uses RCU for lock-free access
     */
    bool has_group(std::string_view name) const TANH_NONBLOCKING_FUNCTION;

    // ── Parameter creation ──────────────────────────────────────────────

    /**
     * @brief Creates a parameter with a ParameterDefinition.
     *
     * Creates intermediate groups as needed. Throws if the parameter
     * already exists.
     *
     * @param path The parameter path (e.g., "oscillator.frequency")
     * @param def The parameter definition (moved into the record)
     *
     * @throws std::invalid_argument if the parameter already exists
     * @warning NOT real-time safe - allocates memory
     */
    void create(std::string_view path, ParameterDefinition def);

    /**
     * @brief Creates a parameter with an initial value and default definition.
     *
     * Creates intermediate groups as needed. Throws if the parameter
     * already exists.
     *
     * @tparam T Value type (double, float, int, bool, std::string, or
     *         ParameterDefinition subclasses)
     * @param path The parameter path
     * @param value The initial value
     *
     * @throws std::invalid_argument if the parameter already exists
     * @warning NOT real-time safe - allocates memory
     */
    template <typename T>
    void create(std::string_view path, const T& value);

    /**
     * @brief Creates a string parameter from a C-string.
     * @copydetails create()
     */
    void create(std::string_view path, const char* value);

    // ── Parameter update ────────────────────────────────────────────────

    /**
     * @brief Sets a parameter value at the specified path.
     *
     * Updates an existing parameter. Throws if the parameter doesn't exist.
     * Always notifies listeners. Use ParameterHandle::store() for silent writes.
     *
     * @tparam T Parameter type (double, float, int, bool, std::string)
     * @param path The parameter path
     * @param value The value to set
     * @param source Source listener for strategy-based notification filtering
     *
     * @throws StateKeyNotFoundException if parameter doesn't exist
     * @throws StateGroupNotFoundException if intermediate group doesn't exist
     *
     * @warning NOT real-time safe - may allocate for string parameters
     */
    template <typename T>
    void set(std::string_view path, const T& value, ParameterListener* source = nullptr);

    /**
     * @brief Sets a string parameter from a C-string.
     * @copydetails set()
     */
    void set(std::string_view path, const char* value, ParameterListener* source = nullptr);

    // ── Parameter access ────────────────────────────────────────────────

    /**
     * @brief Gets a parameter value from the specified path.
     *
     * @tparam T Return type (double, float, int, bool, or std::string)
     * @param path The parameter path
     * @param allow_blocking If true, disables real-time sanitizer checks
     *
     * @return The parameter value converted to type T
     *
     * @throws StateKeyNotFoundException if the parameter doesn't exist
     * @throws StateGroupNotFoundException if an intermediate group doesn't exist
     *
     * @note **REAL-TIME SAFE** for numeric types (double, float, int, bool)
     */
    template <typename T>
    T get(std::string_view path, bool allow_blocking = false) const TANH_NONBLOCKING_FUNCTION;

    /**
     * @brief Gets a Parameter object for the specified path.
     * @param path The parameter path (e.g., "oscillator.frequency")
     * @return Parameter object providing access to the parameter
     * @throws StateKeyNotFoundException if the parameter doesn't exist
     * @throws StateGroupNotFoundException if an intermediate group doesn't exist
     * @warning NOT real-time safe (BLOCKING) - creates a Parameter object
     */
    Parameter get_parameter(std::string_view path) const;

    /**
     * @brief Gets all parameters in this group and its subgroups.
     * @return Map of parameter paths to Parameter objects
     * @warning NOT real-time safe - allocates memory for the returned map
     */
    std::map<std::string, Parameter> get_parameters() const;

    /**
     * @brief Gets the type of a parameter at the specified path.
     * @param path The parameter path
     * @return ParameterType enum indicating the parameter's type
     * @throws StateKeyNotFoundException if the parameter doesn't exist
     * @note **REAL-TIME SAFE** - uses RCU for lock-free access
     */
    ParameterType get_parameter_type(std::string_view path) const TANH_NONBLOCKING_FUNCTION;

    /**
     * @brief Gets a lightweight handle for real-time safe per-sample parameter access.
     *
     * @tparam T Numeric type: double, float, int, or bool
     * @param path The parameter path (must already exist)
     *
     * @return ParameterHandle<T> pointing to the parameter's atomic cache entry
     *
     * @throws StateKeyNotFoundException if the parameter doesn't exist
     * @throws StateGroupNotFoundException if an intermediate group doesn't exist
     * @throws std::invalid_argument if T doesn't match the parameter's native type
     *
     * @warning NOT real-time safe — call during setup, then use the handle
     *          on the real-time thread.
     */
    template <typename T>
    ParameterHandle<T> get_handle(std::string_view path) const;

    // ── Gesture ─────────────────────────────────────────────────────────

    /**
     * @brief Sets the gesture state for a parameter.
     *
     * @param path The parameter path
     * @param gesture true to begin gesture, false to end
     */
    void set_gesture(std::string_view path, bool gesture);

    // ── Listener management ─────────────────────────────────────────────

    /**
     * @brief Adds a listener for parameter changes in this group.
     * @param listener Pointer to the listener (must remain valid while registered)
     * @warning NOT real-time safe - modifies listener list
     */
    void add_listener(ParameterListener* listener);

    /**
     * @brief Removes a previously added listener.
     * @param listener Pointer to the listener to remove
     * @warning NOT real-time safe - modifies listener list
     */
    void remove_listener(ParameterListener* listener);

    /**
     * @brief Manually triggers notifications for a parameter.
     * @param path The parameter path to notify about
     * @warning NOT real-time safe - invokes listener callbacks
     */
    void notify_parameter_change(std::string_view path);

    // ── State management ────────────────────────────────────────────────

    /**
     * @brief Clears all parameters and subgroups from this group.
     * @warning NOT real-time safe - modifies RCU-protected data structures
     */
    virtual void clear();

    /**
     * @brief Clears all subgroups from this group.
     * @warning NOT real-time safe - modifies RCU-protected data structures
     */
    void clear_groups();

    /**
     * @brief Checks if this group contains any parameters or subgroups.
     * @return true if the group is empty, false otherwise
     * @note **REAL-TIME SAFE** - uses RCU for lock-free access
     */
    virtual bool is_empty() const TANH_NONBLOCKING_FUNCTION;

    // ── Hierarchy ───────────────────────────────────────────────────────

    StateGroup* get_parent() const { return m_parent; }
    State* get_root_state() const { return m_root_state; }

    /**
     * @brief Gets the full dot-separated path from the root to this group.
     * @return String view of the full path
     * @note **REAL-TIME SAFE** - uses pre-allocated string buffers
     */
    std::string_view get_full_path() const TANH_NONBLOCKING_FUNCTION;

private:
    friend class State;
    friend class Parameter;

    /**
     * @brief Notifies all listeners about a parameter change.
     *
     * Propagates notifications up the group hierarchy to parent groups.
     * If source is provided, source->m_strategy controls which listeners
     * receive the callback.
     *
     * @param param The Parameter object
     * @param source Source listener for strategy-based filtering
     */
    void notify_listeners(const Parameter& param, ParameterListener* source = nullptr) const;

    std::pair<StateGroup*, std::string_view> resolve_path(std::string_view path) const;
    std::pair<StateGroup*, std::string_view> resolve_path_create(std::string_view path);

    State* m_root_state;
    StateGroup* m_parent;
    std::string m_name;

    /**
     * @brief Listener data structure for RCU protection.
     */
    struct ListenerData {
        std::vector<ParameterListener*> m_object_listeners;

        ListenerData() = default;
        ListenerData(const ListenerData& other) = default;
    };

    mutable RCU<ListenerData> m_listeners_rcu;

    using GroupMap = std::map<std::string, std::shared_ptr<StateGroup>>;
    mutable RCU<GroupMap> m_groups_rcu;

    virtual void ensure_thread_registered();
    void ensure_child_groups_registered();

    static std::unordered_set<const StateGroup*>& t_registered_states() noexcept {
        static thread_local std::unordered_set<const StateGroup*> s;
        return s;
    }
};

}  // namespace thl
