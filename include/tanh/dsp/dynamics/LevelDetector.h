#pragma once

#include <tanh/core/AbiNamespace.h>
#include <tanh/dsp/utils/TimeConstant.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace thl::inline THL_ABI_NAMESPACE::dsp::dynamics {

/// What a level detector measures.
enum class DetectorMode : uint8_t {
    Peak,  ///< the instantaneous level |x|: the gain smoother's attack and release are the
           ///< ballistics
    Rms,   ///< the mean square over a one-pole window
};

/**
 * @brief The level of one channel as a power (a squared amplitude), sample by sample.
 *
 * Peak gives x², Rms the mean square through a one-pole of time constant rms window: a sine of
 * peak A reads A² / 2, -3.01 dB under its peak, once the window is long against its period.
 * Linking channels happens on these powers (link_powers(), StereoLink.h), the dB value once
 * after it (utils::power_to_db(), Decibels.h).
 *
 * Threads: prepare() and set_rms_window_ms() compute a coefficient; all is real-time safe.
 */
class LevelDetector {
public:
    void prepare(double sample_rate) {
        m_sample_rate = sample_rate;
        update_coefficient();
        reset();
    }

    /// A change clears the RMS window, so it does not resume from what it held before.
    void set_mode(DetectorMode mode) TANH_NONBLOCKING_FUNCTION {
        if (mode != m_mode) { reset(); }
        m_mode = mode;
    }
    [[nodiscard]] DetectorMode mode() const { return m_mode; }

    /// The RMS window's time constant in ms (at least one sample).
    void set_rms_window_ms(float window_ms) TANH_NONBLOCKING_FUNCTION {
        if (window_ms == m_rms_window_ms) { return; }
        m_rms_window_ms = window_ms;
        update_coefficient();
    }

    void reset() TANH_NONBLOCKING_FUNCTION { m_mean_square = 0.0f; }

    /// The power at the sample @p x.
    /// A non-finite sample reads as silence, so it cannot leave the RMS window NaN for good.
    [[nodiscard]] float process(float x) TANH_NONBLOCKING_FUNCTION {
        const float square = std::isfinite(x) ? x * x : 0.0f;
        if (m_mode == DetectorMode::Peak) { return square; }
        m_mean_square = square + (m_coefficient * (m_mean_square - square));
        return m_mean_square;
    }

private:
    void update_coefficient() TANH_NONBLOCKING_FUNCTION {
        // At least one sample: a shorter window would be no smoothing at all.
        m_coefficient = utils::one_pole_pole(
            std::max(1.0, utils::ms_to_samples(m_rms_window_ms, m_sample_rate)));
    }

    double m_sample_rate = 48000.0;
    float m_rms_window_ms = 10.0f;
    float m_coefficient = 0.0f;
    float m_mean_square = 0.0f;
    DetectorMode m_mode = DetectorMode::Peak;
};

}  // namespace thl::inline THL_ABI_NAMESPACE::dsp::dynamics
