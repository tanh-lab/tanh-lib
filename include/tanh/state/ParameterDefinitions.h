#pragma once

#include <string>
#include <vector>
#include <algorithm>

namespace thl {

enum class PluginParamType {
    ParamFloat,
    ParamInt,
    ParamBool,
    ParamChoice
};

struct Range {
    float m_min;
    float m_max;
    float m_step;
    float m_skew;
    
    Range()
        : m_min(0.0f), m_max(1.0f), m_step(0.01f), m_skew(1.0f) {}
    
    Range(
        float min,
        float max,
        float step = 0.01f,
        float skew = 1.0f
    ) : m_min(min), m_max(max), m_step(step), m_skew(skew) {}
    
    Range(
        int min,
        int max,
        int step = 1
    ) : m_min(static_cast<float>(min)),
        m_max(static_cast<float>(max)),
        m_step(static_cast<float>(step)),
        m_skew(1.0f) {}
    
    int min_int() const { return static_cast<int>(m_min); }
    int max_int() const { return static_cast<int>(m_max); }
    int step_int() const { return static_cast<int>(m_step); }
    
    static Range Bool() {
        return {0.0f, 1.0f, 1.0f, 1.0f};
    }
};

struct ParameterDefinition {
    const std::string m_name;
    const PluginParamType m_type = PluginParamType::ParamFloat;
    const Range m_range;
    const float m_default_value = 0.0f;
    const size_t m_decimal_places = 0;
    const bool m_automation = true;
    const bool m_modulation = true;
    const std::vector<std::string> m_data;
    
    ParameterDefinition(
        std::string name,
        PluginParamType type,
        Range range,
        float default_value,
        size_t decimal_places = 0,
        bool automation = true,
        bool modulation = true,
        std::vector<std::string> data = {}
    ) : m_name(std::move(name)),
        m_type(type),
        m_range(range),
        m_default_value(default_value),
        m_decimal_places(decimal_places),
        m_automation(automation),
        m_modulation(modulation),
        m_data(std::move(data)) {}
    
    float as_float() const { return m_default_value; }
    int as_int() const { return static_cast<int>(m_default_value); }
    bool as_bool() const { return m_default_value != 0.0f; }
};

struct ParameterFloat : public ParameterDefinition {
    ParameterFloat(
        std::string name,
        Range range,
        float default_val,
        size_t decimal_places = 2,
        bool automation = true,
        bool modulation = true
    ) : ParameterDefinition(std::move(name), PluginParamType::ParamFloat,
                           range, default_val, decimal_places, automation, modulation) {}
};

struct ParameterInt : public ParameterDefinition {
    ParameterInt(
        std::string name,
        Range range,
        int default_val,
        bool automation = true,
        bool modulation = true
    ) : ParameterDefinition(std::move(name), PluginParamType::ParamInt,
                           range, static_cast<float>(default_val), 0, automation, modulation) {}
};

struct ParameterBool : public ParameterDefinition {
    ParameterBool(
        std::string name,
        bool default_val = false,
        bool automation = true,
        bool modulation = false
    ) : ParameterDefinition(std::move(name), PluginParamType::ParamBool,
                           Range::Bool(), default_val ? 1.0f : 0.0f, 0, automation, modulation) {}
};

struct ParameterChoice : public ParameterDefinition {
    ParameterChoice(
        std::string name,
        std::vector<std::string> choices,
        int default_val = 0,
        bool automation = true,
        bool modulation = false
    ) : ParameterDefinition(std::move(name), PluginParamType::ParamChoice,
                           Range(0, static_cast<int>(choices.size()) - 1, 1),
                           static_cast<float>(default_val), 0, automation, modulation, std::move(choices)) {}
};

} // namespace thl
