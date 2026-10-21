#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>

namespace thl::dsp::utils {

enum class FilterType : uint8_t { LowPass, HighPass, AllPass };

/**
 * Butterworth low-, high- or all-pass of order 1 to k_max_order, as cascaded TPT sections: one
 * 1st-order section if the order is odd, then order / 2 2nd-order sections with
 * Q_k = 1 / (2·cos(π·(2k + 1 + order mod 2) / (2·order))).
 * The cutoff is limited to just below Nyquist and prewarped: g = tan(π·f / fs).
 * The all-pass has the Butterworth poles and a flat magnitude: the phase of a Linkwitz-Riley
 * crossover of twice the order (low + high).
 *
 * Call prepare() before use.
 */
class Butterworth {
public:
    static constexpr size_t k_max_order = 4;
    static constexpr float k_max_frequency_ratio = 0.497f;  // of the sample rate

    Butterworth() = default;

    void prepare(float frequency, float sample_rate, size_t order, FilterType filter_type) {
        assert(order >= 1 && order <= k_max_order);
        m_sample_rate = sample_rate;
        m_order = order;
        m_filter_type = filter_type;
        for (size_t k = 0; k < m_order / 2; ++k) {
            const double angle = std::numbers::pi * static_cast<double>(2 * k + 1 + m_order % 2) /
                                 static_cast<double>(2 * m_order);
            m_damping[k] = static_cast<float>(2.0 * std::cos(angle));
        }
        set_frequency(frequency);
        reset();
    }

    void set_frequency(float frequency) {
        frequency = std::min(frequency, k_max_frequency_ratio * m_sample_rate);
        m_g = std::tan(std::numbers::pi_v<float> * frequency / m_sample_rate);
    }

    void reset() { m_states.fill(0.0f); }

    float process(float x) {
        assert(m_order > 0);
        const size_t odd = m_order % 2;
        if (odd == 1) {
            const float v = (x - m_states[0]) * m_g / (1.0f + m_g);
            const float low = v + m_states[0];
            m_states[0] = low + v;
            switch (m_filter_type) {
                case FilterType::LowPass: x = low; break;
                case FilterType::HighPass: x = x - low; break;
                case FilterType::AllPass: x = 2.0f * low - x; break;
            }
        }
        for (size_t k = 0; k < m_order / 2; ++k) {
            float& s1 = m_states[odd + 2 * k];
            float& s2 = m_states[odd + 2 * k + 1];
            const float high =
                (x - (m_damping[k] + m_g) * s1 - s2) / (1.0f + m_damping[k] * m_g + m_g * m_g);
            const float band = m_g * high + s1;
            s1 = m_g * high + band;
            const float low = m_g * band + s2;
            s2 = m_g * band + low;
            switch (m_filter_type) {
                case FilterType::LowPass: x = low; break;
                case FilterType::HighPass: x = high; break;
                case FilterType::AllPass: x = x - 2.0f * m_damping[k] * band; break;
            }
        }
        return x;
    }

    Butterworth(const Butterworth&) = delete;
    Butterworth& operator=(const Butterworth&) = delete;

private:
    float m_sample_rate = 48000.0f;
    float m_g = 0.0f;
    size_t m_order = 0;
    FilterType m_filter_type = FilterType::LowPass;
    std::array<float, k_max_order / 2> m_damping{};  // 1/Q per 2nd-order section
    std::array<float, k_max_order> m_states{};
};

}  // namespace thl::dsp::utils
