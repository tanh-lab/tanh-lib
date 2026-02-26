#pragma once

#include <cmath>

namespace thl::dsp::resonator {

class ParamSmoother {
public:
    void prepare(double sampleRate, float timeInSeconds) {
        m_coeff = std::exp(-1.0f / (static_cast<float>(sampleRate) * timeInSeconds));
    }

    void set_target(float target) { m_target = target; }

    float skip(int numSamples) {
        m_current = m_target + (m_current - m_target) * std::pow(m_coeff, static_cast<float>(numSamples));
        return m_current;
    }

    float get_current_value() const { return m_current; }

private:
    float m_current = 0.0f;
    float m_target = 0.0f;
    float m_coeff = 0.0f;
};

} // namespace thl::dsp::resonator
