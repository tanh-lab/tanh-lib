#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "ParameterDefinitions.h"
#include "path_helpers.h"
#include "tanh/utils/RealtimeSanitizer.h"

namespace thl {

class State;
class StateGroup;
class ParameterListener;

// Parameter type enum
enum class ParameterType { Double, Float, Int, Bool, String, Unknown };

// different strategies for notification
enum class NotifyStrategies {all, none, others, self};

// Internal parameter storage - using atomic values for all numeric types
struct ParameterData {
    std::atomic<ParameterType> type;
    std::atomic<double> double_value;
    std::atomic<float> float_value;
    std::atomic<int> int_value;
    std::atomic<bool> bool_value;
    std::string string_value; // Requires mutex protection for modification
    std::unique_ptr<ParameterDefinition> parameter_definition;
    
    // Default constructor with initialization
    ParameterData() : 
        type(ParameterType::Double),
        double_value(0.0),
        float_value(0.0f),
        int_value(0),
        bool_value(false),
        parameter_definition(nullptr) {}
        
    // Custom copy constructor for RCU map copying
    ParameterData(const ParameterData& other) :
        type(other.type.load()),
        double_value(other.double_value.load()),
        float_value(other.float_value.load()),
        int_value(other.int_value.load()),
        bool_value(other.bool_value.load()),
        string_value(other.string_value),
        parameter_definition(other.parameter_definition ? std::make_unique<ParameterDefinition>(*other.parameter_definition) : nullptr) {}
        
    // Custom assignment operator
    ParameterData& operator=(const ParameterData& other) {
        if (this != &other) {
            type.store(other.type.load());
            double_value.store(other.double_value.load());
            float_value.store(other.float_value.load());
            int_value.store(other.int_value.load());
            bool_value.store(other.bool_value.load());
            string_value = other.string_value;
            parameter_definition = other.parameter_definition ? std::make_unique<ParameterDefinition>(*other.parameter_definition) : nullptr;
        }
        return *this;
    }
    
    // Move operations
    ParameterData(ParameterData&& other) noexcept :
        type(other.type.load()),
        double_value(other.double_value.load()),
        float_value(other.float_value.load()),
        int_value(other.int_value.load()),
        bool_value(other.bool_value.load()),
        string_value(std::move(other.string_value)),
        parameter_definition(std::move(other.parameter_definition)) {}
        
    ParameterData& operator=(ParameterData&& other) noexcept {
        if (this != &other) {
            type.store(other.type.load());
            double_value.store(other.double_value.load());
            float_value.store(other.float_value.load());
            int_value.store(other.int_value.load());
            bool_value.store(other.bool_value.load());
            string_value = std::move(other.string_value);
            parameter_definition = std::move(other.parameter_definition);
        }
        return *this;
    }
};

/**
 * @class Parameter
 * @brief Object-oriented wrapper for parameter access with type conversion support.
 * 
 * Parameter provides a convenient interface for accessing parameter values with
 * automatic type conversion. It wraps a reference to the underlying State and
 * parameter key.
 * 
 * @section rt_safety Real-Time Safety
 * 
 * - Type checking methods (`is_double()`, `is_float()`, etc.) are **real-time safe**
 * - `to<T>()` for numeric types (double, float, int, bool) is **real-time safe**
 * - `to<std::string>()` requires `allow_blocking=true` as it may allocate memory
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
     * @param allow_blocking If true, allows blocking operations (required for string type).
     *                       When false and T is std::string, throws BlockingException.
     * 
     * @return The parameter value converted to type T
     * 
     * @throws BlockingException if T is std::string and allow_blocking is false
     * @throws StateKeyNotFoundException if the parameter doesn't exist
     * 
     * @note **REAL-TIME SAFE** for numeric types (double, float, int, bool)
     * @warning String type requires `allow_blocking=true` as it may allocate memory
     * 
     * @par Example
     * @code
     * Parameter param = state.get_parameter("volume");
     * double vol = param.to<double>();           // Real-time safe
     * std::string str = param.to<std::string>(true);  // Requires allow_blocking=true
     * @endcode
     */
    template<typename T>
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
    void notify(NotifyStrategies strategy = NotifyStrategies::all, ParameterListener* source = nullptr) const;
    
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
     * @return Pointer to the ParameterDefinition, or nullptr if not set
     * 
     * @note **REAL-TIME SAFE** - returns a pointer to existing data
     */
    [[nodiscard]] ParameterDefinition* get_definition() const TANH_NONBLOCKING_FUNCTION;

private:
    friend class State;
    friend class StateGroup;
    
    // Private constructors - only State/StateGroup can create Parameters
    Parameter(const State* state, std::string_view key);
    Parameter(const StateGroup* group, std::string_view key, const State* rootState);
    
    // State reference and parameter key
    const State* m_state;
    std::string m_key;
};

} // namespace thl
