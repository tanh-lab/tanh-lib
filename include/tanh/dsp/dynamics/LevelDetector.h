#pragma once

#include <tanh/core/AbiNamespace.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <span>

namespace thl::inline THL_ABI_NAMESPACE::dsp::dynamics {

/// What a level detector measures.
enum class DetectorMode : uint8_t {
    Peak,  ///< the instantaneous level |x|: the gain smoother's attack and release are the
           ///< ballistics
    Rms,   ///< the mean square over a one-pole window
};

/// How the channels of a dynamics processor share one gain.
enum class StereoLink : uint8_t {
    Linked,    ///< the loudest channel drives all (the image stays where it is)
    Average,   ///< the channels' mean power drives all
    DualMono,  ///< every channel its own gain
};

/// The level in dB of a power (a squared amplitude), at least k_floor_db.
inline constexpr float k_floor_db = -120.0f;
[[nodiscard]] inline float power_to_db(float power) TANH_NONBLOCKING_FUNCTION {
    constexpr float k_floor_power = 1e-12f;  // k_floor_db
    return 10.0f * std::log10(std::max(power, k_floor_power));
}

/// The one power of linked channels: their maximum (Linked) or mean (Average). Not for DualMono,
/// which links nothing.
[[nodiscard]] inline float link_powers(StereoLink link,
                                       std::span<const float> powers) TANH_NONBLOCKING_FUNCTION {
    assert(link != StereoLink::DualMono);
    if (powers.empty()) { return 0.0f; }
    if (link == StereoLink::Linked) { return *std::ranges::max_element(powers); }
    float sum = 0.0f;
    for (const float power : powers) { sum += power; }
    return sum / static_cast<float>(powers.size());
}

/**
 * @brief The level of one channel as a power (a squared amplitude), sample by sample.
 *
 * Peak gives x², Rms the mean square through a one-pole of time constant rms window: a sine of
 * peak A reads A² / 2, -3.01 dB under its peak, once the window is long against its period.
 * Linking channels happens on these powers (link_powers()), the dB value once after it
 * (power_to_db()).
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

    void set_mode(DetectorMode mode) TANH_NONBLOCKING_FUNCTION { m_mode = mode; }
    [[nodiscard]] DetectorMode mode() const { return m_mode; }

    /// The RMS window's time constant in ms (at least one sample).
    void set_rms_window_ms(float window_ms) TANH_NONBLOCKING_FUNCTION {
        if (window_ms == m_rms_window_ms) { return; }
        m_rms_window_ms = window_ms;
        update_coefficient();
    }

    void reset() TANH_NONBLOCKING_FUNCTION { m_mean_square = 0.0f; }

    /// The power at the sample @p x.
    [[nodiscard]] float process(float x) TANH_NONBLOCKING_FUNCTION {
        const float square = x * x;
        if (m_mode == DetectorMode::Peak) { return square; }
        m_mean_square = square + (m_coefficient * (m_mean_square - square));
        return m_mean_square;
    }

private:
    void update_coefficient() TANH_NONBLOCKING_FUNCTION {
        const double samples =
            std::max(1.0, static_cast<double>(m_rms_window_ms) * 0.001 * m_sample_rate);
        m_coefficient = static_cast<float>(std::exp(-1.0 / samples));
    }

    double m_sample_rate = 48000.0;
    float m_rms_window_ms = 10.0f;
    float m_coefficient = 0.0f;
    float m_mean_square = 0.0f;
    DetectorMode m_mode = DetectorMode::Peak;
};

}  // namespace thl::inline THL_ABI_NAMESPACE::dsp::dynamics
