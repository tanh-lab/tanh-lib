#pragma once

#include <cmath>

namespace thl::dsp::utils {

class ParamSmoother {
public:
    void prepare(double sample_rate, float time_in_seconds) {
        m_coeff = std::exp(-1.0f / (static_cast<float>(sample_rate) * time_in_seconds));
    }

    void set_target(float target) { m_target = target; }

    float skip(int num_samples) {
        m_current =
            m_target + (m_current - m_target) * std::pow(m_coeff, static_cast<float>(num_samples));
        return m_current;
    }

    float get_current_value() const { return m_current; }

private:
    float m_current = 0.0f;
    float m_target = 0.0f;
    float m_coeff = 0.0f;
};

}  // namespace thl::dsp::utils
