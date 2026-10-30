#pragma once

#include <tanh/dsp/filter/SvfCoefficients.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>

namespace thl::dsp::filter {

/**
 * Butterworth low-, high- or all-pass of order 1 to k_max_order: one 1st-order TPT section if the
 * order is odd, then order / 2 trapezoidal SVF sections (SvfTick) with
 * Q_k = 1 / (2·cos(π·(2k + 1 + order mod 2) / (2·order))). States in double, so a low cutoff at a
 * high sample rate keeps its precision.
 * The cutoff is prewarped with prewarp(), which limits it to just below Nyquist.
 * The all-pass has the Butterworth poles and a flat magnitude: the phase of a Linkwitz-Riley
 * crossover of twice the order (low + high).
 *
 * Call prepare() before use. prepare() is not real-time safe in spirit (it sets the order);
 * set_frequency(), reset() and process() are.
 */
class Butterworth {
public:
    enum class Response : uint8_t { LowPass, HighPass, AllPass };

    static constexpr size_t k_max_order = 4;
    static constexpr float k_max_frequency_ratio = 0.497f;  // of the sample rate
    static constexpr float k_min_frequency = 1.0f;          // Hz; also for a non-finite one

    /// The integrator gain g = tan(π·f / fs) of the sections, with the cutoff @p frequency limited
    /// to [k_min_frequency, k_max_frequency_ratio · @p sample_rate].
    static float prewarp(float frequency, float sample_rate) TANH_NONBLOCKING_FUNCTION {
        const float f = std::isfinite(frequency) ? frequency : k_min_frequency;
        const float limited = std::clamp(f, k_min_frequency, k_max_frequency_ratio * sample_rate);
        return std::tan(std::numbers::pi_v<float> * limited / sample_rate);
    }

    Butterworth() = default;

    void prepare(float frequency, float sample_rate, size_t order, Response response) {
        assert(order >= 1 && order <= k_max_order);
        m_sample_rate = sample_rate;
        m_order = order;
        m_response = response;
        for (size_t k = 0; k < m_order / 2; ++k) {
            const double angle = std::numbers::pi * static_cast<double>(2 * k + 1 + m_order % 2) /
                                 static_cast<double>(2 * m_order);
            m_damping[k] = 2.0 * std::cos(angle);
        }
        set_frequency(frequency);
        reset();
    }

    void set_frequency(float frequency) TANH_NONBLOCKING_FUNCTION {
        m_g = prewarp(frequency, m_sample_rate);
    }

    void reset() TANH_NONBLOCKING_FUNCTION {
        m_first_order = 0.0;
        for (auto& s : m_states) { s.reset(); }
    }

    float process(float x) TANH_NONBLOCKING_FUNCTION { return process(x, m_g); }

    /// Processes with @p g, from prewarp(), instead of the frequency of set_frequency(): filters at
    /// the same frequency can share one tan() call.
    float process(float x, float g) TANH_NONBLOCKING_FUNCTION {
        assert(m_order > 0);
        if (m_order % 2 == 1) {
            const double gd = g;
            const double v = (static_cast<double>(x) - m_first_order) * gd / (1.0 + gd);
            const double low = v + m_first_order;
            m_first_order = low + v;
            switch (m_response) {
                case Response::LowPass: x = static_cast<float>(low); break;
                case Response::HighPass:
                    x = static_cast<float>(static_cast<double>(x) - low);
                    break;
                case Response::AllPass:
                    x = static_cast<float>((2.0 * low) - static_cast<double>(x));
                    break;
            }
        }
        for (size_t k = 0; k < m_order / 2; ++k) {
            x = SvfTick(section(g, m_damping[k])).process(m_states[k], x);
        }
        return x;
    }

    Butterworth(const Butterworth&) = delete;
    Butterworth& operator=(const Butterworth&) = delete;

private:
    /// One 2nd-order section with damping @p k as a mix of the SVF's taps.
    [[nodiscard]] SvfCoefficients section(float g, double k) const TANH_NONBLOCKING_FUNCTION {
        SvfCoefficients c{.m_g = g, .m_k = k, .m_m0 = 0.0, .m_m1 = 0.0, .m_m2 = 0.0};
        switch (m_response) {
            case Response::LowPass: c.m_m2 = 1.0; break;
            case Response::HighPass:
                c.m_m0 = 1.0;
                c.m_m1 = -k;
                c.m_m2 = -1.0;
                break;
            case Response::AllPass:
                c.m_m0 = 1.0;
                c.m_m1 = -2.0 * k;
                break;
        }
        return c;
    }

    float m_sample_rate = 48000.0f;
    float m_g = 0.0f;
    size_t m_order = 0;
    Response m_response = Response::LowPass;
    std::array<double, k_max_order / 2> m_damping{};  // 1/Q per 2nd-order section
    double m_first_order = 0.0;
    std::array<SvfState, k_max_order / 2> m_states{};
};

}  // namespace thl::dsp::filter
