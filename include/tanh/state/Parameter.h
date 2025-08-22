#pragma once

#include <atomic>
#include <string>

#include "path_helpers.h"

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
    
    // Default constructor with initialization
    ParameterData() : 
        type(ParameterType::Double),
        double_value(0.0),
        float_value(0.0f),
        int_value(0),
        bool_value(false) {}
        
    // Custom copy constructor for RCU map copying
    ParameterData(const ParameterData& other) :
        type(other.type.load()),
        double_value(other.double_value.load()),
        float_value(other.float_value.load()),
        int_value(other.int_value.load()),
        bool_value(other.bool_value.load()),
        string_value(other.string_value) {}
        
    // Custom assignment operator
    ParameterData& operator=(const ParameterData& other) {
        if (this != &other) {
            type.store(other.type.load());
            double_value.store(other.double_value.load());
            float_value.store(other.float_value.load());
            int_value.store(other.int_value.load());
            bool_value.store(other.bool_value.load());
            string_value = other.string_value;
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
        string_value(std::move(other.string_value)) {}
        
    ParameterData& operator=(ParameterData&& other) noexcept {
        if (this != &other) {
            type.store(other.type.load());
            double_value.store(other.double_value.load());
            float_value.store(other.float_value.load());
            int_value.store(other.int_value.load());
            bool_value.store(other.bool_value.load());
            string_value = std::move(other.string_value);
        }
        return *this;
    }
};

// Parameter class for object-oriented parameter access
class Parameter {
public:
    // Conversion methods
    template<typename T>
    T to() const;

    // Type checking
    ParameterType get_type() const;
    bool is_double() const;
    bool is_float() const;
    bool is_int() const;
    bool is_bool() const;
    bool is_string() const;
    
    // Notification
    void notify(NotifyStrategies strategy = NotifyStrategies::all, ParameterListener* source = nullptr) const;
    
    // Get the path for this parameter
    std::string get_path() const;
    
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
