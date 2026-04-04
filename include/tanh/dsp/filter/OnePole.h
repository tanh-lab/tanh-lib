#pragma once

#include <cmath>
#include <tanh/core/Numbers.h>

#include <tanh/dsp/DspTypes.h>

namespace thl::dsp::filter {

enum class FilterMode {
    LowPass,
    BandPass,
    BandPassNormalized,
    HighPass,
};

namespace detail {
constexpr float k_pi_f = std::numbers::pi_v<float>;
constexpr float k_pi_pow2 = k_pi_f * k_pi_f;
constexpr float k_pi_pow3 = k_pi_pow2 * k_pi_f;
constexpr float k_pi_pow5 = k_pi_pow3 * k_pi_pow2;
constexpr float k_pi_pow7 = k_pi_pow5 * k_pi_pow2;
constexpr float k_pi_pow9 = k_pi_pow7 * k_pi_pow2;
constexpr float k_pi_pow11 = k_pi_pow9 * k_pi_pow2;
}  // namespace detail

// Topology-Preserving Transform (TPT) one-pole filter.
//
// Obtained by applying the bilinear transform to the continuous-time transfer
// function H(s) = g / (s + g) and then prewarping the cutoff frequency so
// that the analogue and digital responses match exactly at the target
// frequency. This yields the trapezoidal integrator form, which is
// unconditionally stable and free of the frequency-warping artefacts found
// in naive digital one-pole implementations.
//
// The prewarping step maps the desired cutoff f to the analogue prototype
// frequency:  w_c = (2/T) * tan(pi * f / f_s)
//
// The static tan() helper computes the prewarped integrator gain
// g = tan(pi * f / f_s) at several accuracy/cost trade-offs and is shared
// with Svf.
class OnePole {
public:
    void reset() { m_state = 0.0f; }

    template <Approximation approximation>
    static float tan(float f) {
        if constexpr (approximation == Approximation::Exact) {
            f = f < 0.497f ? f : 0.497f;
            return std::tan(detail::k_pi_f * f);
        } else if constexpr (approximation == Approximation::Dirty) {
            const float a = 3.736e-01f * detail::k_pi_pow3;
            return f * (detail::k_pi_f + a * f * f);
        } else if constexpr (approximation == Approximation::Fast) {
            const float a = 3.260e-01f * detail::k_pi_pow3;
            const float b = 1.823e-01f * detail::k_pi_pow5;
            const float f2 = f * f;
            return f * (detail::k_pi_f + f2 * (a + b * f2));
        } else {
            const float a = 3.333314036e-01f * detail::k_pi_pow3;
            const float b = 1.333923995e-01f * detail::k_pi_pow5;
            const float c = 5.33740603e-02f * detail::k_pi_pow7;
            const float d = 2.900525e-03f * detail::k_pi_pow9;
            const float e = 9.5168091e-03f * detail::k_pi_pow11;
            const float f2 = f * f;
            return f * (detail::k_pi_f + f2 * (a + f2 * (b + f2 * (c + f2 * (d + f2 * e)))));
        }
    }

    // Set the cutoff frequency as a normalised value f = f_hz / f_s (range
    // 0..0.5). Computes the prewarped integrator gain g = tan(pi * f / f_s)
    // and caches 1/(1+g) for use in process().
    template <Approximation approximation>
    void set_f(float f) {
        m_g = tan<approximation>(f);
        m_gi = 1.0f / (1.0f + m_g);
    }

    template <FilterMode mode>
    float process(float in) {
        const float lp = (m_g * in + m_state) * m_gi;
        m_state = m_g * (in - lp) + lp;

        if constexpr (mode == FilterMode::LowPass) {
            return lp;
        } else if constexpr (mode == FilterMode::HighPass) {
            return in - lp;
        } else {
            return 0.0f;
        }
    }

private:
    float m_g = 0.0f;
    float m_gi = 1.0f;
    float m_state = 0.0f;
};

}  // namespace thl::dsp::filter
