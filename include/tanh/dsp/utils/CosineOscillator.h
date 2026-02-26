#pragma once

#include <cmath>

namespace thl::dsp::utils {

enum class CosineOscillatorMode {
    Approximate,
    Exact,
};

class CosineOscillator {
public:
    template <CosineOscillatorMode mode>
    void init(float frequency) {
        if constexpr (mode == CosineOscillatorMode::Approximate) {
            init_approximate(frequency);
        } else {
            m_iir_coefficient = 2.0f * std::cos(2.0f * static_cast<float>(M_PI) * frequency);
            m_initial_amplitude = m_iir_coefficient * 0.25f;
        }
        start();
    }

    void init_approximate(float frequency) {
        float sign = 16.0f;
        frequency -= 0.25f;
        if (frequency < 0.0f) {
            frequency = -frequency;
        } else {
            if (frequency > 0.5f) {
                frequency -= 0.5f;
            } else {
                sign = -16.0f;
            }
        }
        m_iir_coefficient = sign * frequency * (1.0f - 2.0f * frequency);
        m_initial_amplitude = m_iir_coefficient * 0.25f;
    }

    void start() {
        m_y1 = m_initial_amplitude;
        m_y0 = 0.5f;
    }

    float value() const {
        return m_y1 + 0.5f;
    }

    float next() {
        const float temp = m_y0;
        m_y0 = m_iir_coefficient * m_y0 - m_y1;
        m_y1 = temp;
        return temp + 0.5f;
    }

private:
    float m_y1 = 0.0f;
    float m_y0 = 0.5f;
    float m_iir_coefficient = 0.0f;
    float m_initial_amplitude = 0.0f;
};

} // namespace thl::dsp::utils
