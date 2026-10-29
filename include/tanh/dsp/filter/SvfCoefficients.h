#pragma once

#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <numbers>

namespace thl::dsp::filter {

/// A second-order section in direct form: H(z) = (b0 + b1 z^-1 + b2 z^-2) / (1 + a1 z^-1 + a2
/// z^-2).
struct BiquadCoefficients {
    double m_b0 = 1.0;
    double m_b1 = 0.0;
    double m_b2 = 0.0;
    double m_a1 = 0.0;
    double m_a2 = 0.0;

    /// True when both poles lie inside the unit circle (the stability triangle).
    [[nodiscard]] bool is_stable() const { return std::abs(m_a1) < 1.0 + m_a2 && m_a2 < 1.0; }
};

/**
 * @brief The coefficients of Cytomic's linear trapezoidal state-variable filter (Simper 2013).
 *
 * Integrator gain g, damping k and the output mix m0 * input + m1 * band + m2 * low. The filter
 * is the bilinear transform of the analogue prototype
 *
 *     H(s) = (m0 s^2 + (m0 k + m1) s + m0 + m2) / (s^2 + k s + 1),   s = j tan(pi f / fs) / g,
 *
 * so every stable biquad is one set of these coefficients (from_biquad()), and the filter is
 * stable for every g > 0, k > 0. Coefficients interpolated between two stable sets stay stable,
 * which is what lets SvfRamp move between any two designs without clicks.
 */
struct SvfCoefficients {
    float m_g = 0.0f;
    float m_k = 1.0f;
    float m_m0 = 1.0f;
    float m_m1 = 0.0f;
    float m_m2 = 0.0f;

    /// The filter that passes its input unchanged.
    [[nodiscard]] static constexpr SvfCoefficients identity() { return {}; }

    [[nodiscard]] constexpr bool is_identity() const {
        return m_m0 == 1.0f && m_m1 == 0.0f && m_m2 == 0.0f;
    }

    /// The same filter from a stable biquad (BiquadCoefficients::is_stable()).
    [[nodiscard]] static SvfCoefficients from_biquad(const BiquadCoefficients& c) {
        // The SVF's denominator in z is (1 + k g + g^2) + 2 (g^2 - 1) z^-1 + (1 - k g + g^2) z^-2,
        // its numerator n2 (1 - z^-1)^2 + n1 g (1 - z^-2) + n0 g^2 (1 + z^-1)^2 for
        // H(s) = (n2 s^2 + n1 s + n0) / (s^2 + k s + 1); both sides evaluated at z = 1 and -1.
        const double at_dc = 1.0 + c.m_a1 + c.m_a2;       // 4 g^2 / (1 + k g + g^2)
        const double at_nyquist = 1.0 - c.m_a1 + c.m_a2;  // 4 / (1 + k g + g^2)
        const double g = std::sqrt(at_dc / at_nyquist);
        const double k = 2.0 * (1.0 - c.m_a2) / (at_nyquist * g);
        const double n0 = (c.m_b0 + c.m_b1 + c.m_b2) / at_dc;
        const double n2 = (c.m_b0 - c.m_b1 + c.m_b2) / at_nyquist;
        const double n1 = k * (c.m_b0 - c.m_b2) / (1.0 - c.m_a2);
        return {.m_g = static_cast<float>(g),
                .m_k = static_cast<float>(k),
                .m_m0 = static_cast<float>(n2),
                .m_m1 = static_cast<float>(n1 - (n2 * k)),
                .m_m2 = static_cast<float>(n0 - n2)};
    }

    /// The same filter as a biquad.
    [[nodiscard]] BiquadCoefficients to_biquad() const {
        const double g = m_g;
        const double k = m_k;
        const double n2 = m_m0;
        const double n1 = (static_cast<double>(m_m0) * k) + m_m1;
        const double n0 = static_cast<double>(m_m0) + m_m2;
        const double a0 = 1.0 + (k * g) + (g * g);
        return {.m_b0 = (n2 + (n1 * g) + (n0 * g * g)) / a0,
                .m_b1 = 2.0 * ((n0 * g * g) - n2) / a0,
                .m_b2 = (n2 - (n1 * g) + (n0 * g * g)) / a0,
                .m_a1 = 2.0 * ((g * g) - 1.0) / a0,
                .m_a2 = (1.0 - (k * g) + (g * g)) / a0};
    }

    /// The filter's exact response at @p frequency (Hz) and @p sample_rate.
    [[nodiscard]] std::complex<double> response(double frequency, double sample_rate) const {
        const double w = std::numbers::pi * std::clamp(frequency / sample_rate, 0.0, 0.4999);
        const std::complex<double> s(0.0, std::tan(w) / static_cast<double>(m_g));
        const double k = m_k;
        const std::complex<double> den = (s * s) + (k * s) + 1.0;
        const std::complex<double> num = (static_cast<double>(m_m0) * s * s) +
                                         (((static_cast<double>(m_m0) * k) + m_m1) * s) +
                                         (static_cast<double>(m_m0) + m_m2);
        return num / den;
    }

    /// 20 log10 |response()|, floored at -240 dB.
    [[nodiscard]] double magnitude_db(double frequency, double sample_rate) const {
        if (is_identity()) { return 0.0; }
        return 20.0 * std::log10(std::max(std::abs(response(frequency, sample_rate)), 1e-12));
    }
};

/// The two integrator states of one SVF channel.
struct SvfState {
    float m_ic1 = 0.0f;
    float m_ic2 = 0.0f;

    void reset() TANH_NONBLOCKING_FUNCTION {
        m_ic1 = 0.0f;
        m_ic2 = 0.0f;
    }
};

/// The per-sample gains of a set of SvfCoefficients (one division, shared by every channel).
struct SvfTick {
    float m_a1 = 1.0f;
    float m_a2 = 0.0f;
    float m_a3 = 0.0f;
    float m_m0 = 1.0f;
    float m_m1 = 0.0f;
    float m_m2 = 0.0f;

    explicit SvfTick(const SvfCoefficients& c) TANH_NONBLOCKING_FUNCTION
        : m_a1(1.0f / (1.0f + (c.m_g * (c.m_g + c.m_k)))),
          m_a2(c.m_g* m_a1),
          m_a3(c.m_g* m_a2),
          m_m0(c.m_m0),
          m_m1(c.m_m1),
          m_m2(c.m_m2) {}

    /// One sample through the filter with state @p s.
    [[nodiscard]] float process(SvfState& s, float v0) const TANH_NONBLOCKING_FUNCTION {
        const float v3 = v0 - s.m_ic2;
        const float v1 = (m_a1 * s.m_ic1) + (m_a2 * v3);
        const float v2 = s.m_ic2 + (m_a2 * s.m_ic1) + (m_a3 * v3);
        s.m_ic1 = (2.0f * v1) - s.m_ic1;
        s.m_ic2 = (2.0f * v2) - s.m_ic2;
        return (m_m0 * v0) + (m_m1 * v1) + (m_m2 * v2);
    }
};

/**
 * @brief Moves SvfCoefficients linearly to a target over a fixed number of samples.
 *
 * Shared by every channel of a filter: advance() once per sample, then SvfTick of current() for
 * each channel. A new target starts a new ramp from where the coefficients are.
 */
class SvfRamp {
public:
    /// The length of every ramp, at least one sample.
    void set_ramp_samples(uint32_t samples) { m_ramp_samples = samples > 0 ? samples : 1; }

    /// Jump to @p c, no ramp.
    void set_current(const SvfCoefficients& c) TANH_NONBLOCKING_FUNCTION {
        m_current = c;
        m_target = c;
        m_remaining = 0;
    }

    /// Ramp to @p c over the ramp length.
    void set_target(const SvfCoefficients& c) TANH_NONBLOCKING_FUNCTION {
        m_target = c;
        const auto n = static_cast<float>(m_ramp_samples);
        m_step = {.m_g = (c.m_g - m_current.m_g) / n,
                  .m_k = (c.m_k - m_current.m_k) / n,
                  .m_m0 = (c.m_m0 - m_current.m_m0) / n,
                  .m_m1 = (c.m_m1 - m_current.m_m1) / n,
                  .m_m2 = (c.m_m2 - m_current.m_m2) / n};
        m_remaining = m_ramp_samples;
    }

    /// End the ramp at its target.
    void finish() TANH_NONBLOCKING_FUNCTION { set_current(m_target); }

    /// One sample along the ramp (the last one lands exactly on the target).
    void advance() TANH_NONBLOCKING_FUNCTION {
        if (m_remaining == 0) { return; }
        if (--m_remaining == 0) {
            m_current = m_target;
            return;
        }
        m_current.m_g += m_step.m_g;
        m_current.m_k += m_step.m_k;
        m_current.m_m0 += m_step.m_m0;
        m_current.m_m1 += m_step.m_m1;
        m_current.m_m2 += m_step.m_m2;
    }

    [[nodiscard]] bool ramping() const { return m_remaining > 0; }
    [[nodiscard]] uint32_t remaining() const { return m_remaining; }
    [[nodiscard]] const SvfCoefficients& current() const { return m_current; }
    [[nodiscard]] const SvfCoefficients& target() const { return m_target; }

private:
    SvfCoefficients m_current;
    SvfCoefficients m_target;
    SvfCoefficients m_step;
    uint32_t m_remaining = 0;
    uint32_t m_ramp_samples = 1;
};

}  // namespace thl::dsp::filter
