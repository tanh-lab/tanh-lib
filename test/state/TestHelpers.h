#pragma once

#include "tanh/state/State.h"
#include <string>

using namespace thl;

// Reusable test listener that tracks parameter change notifications
class TestParameterListener : public ParameterListener {
public:
    void on_parameter_changed(const Parameter& param) override {
        m_last_path = param.key();

        // Store the value based on the parameter type
        switch (param.get_type()) {
            case ParameterType::Double: m_last_value_double = param.to<double>(); break;
            case ParameterType::Float: m_last_value_double = param.to<float>(); break;
            case ParameterType::Int: m_last_value_double = param.to<int>(); break;
            case ParameterType::Bool: m_last_value_double = param.to<bool>() ? 1.0 : 0.0; break;
            default: m_last_value_double = 0.0; break;
        }

        m_notification_count++;
    }

    std::string m_last_path;
    double m_last_value_double = 0.0;
    int m_notification_count = 0;
};
