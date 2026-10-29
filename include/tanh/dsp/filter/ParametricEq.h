#pragma once

#include <tanh/core/Exports.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace thl::dsp::filter {

/// What an EqBand does to the spectrum around its frequency.
enum class EqBandType : uint8_t {
    Peak,       ///< bell: boost or cut by the gain, width by the Q
    Notch,      ///< removes the frequency, width by the Q
    LowShelf,   ///< boost or cut below the frequency; the Q shapes the slope (0.707: no bump)
    HighShelf,  ///< boost or cut above the frequency; the Q shapes the slope (0.707: no bump)
    LowCut,     ///< 12 dB/octave high-pass; the Q is its resonance (0.707: Butterworth)
    HighCut,    ///< 12 dB/octave low-pass; the Q is its resonance (0.707: Butterworth)
};

/// One band of a ParametricEq.
struct EqBand {
    EqBandType m_type = EqBandType::Peak;
    float m_frequency = 1000.0f;  ///< Hz, clamped to [10, 0.49 * sample rate]
    float m_gain_db = 0.0f;       ///< Peak and the shelves, clamped to [-48, 48]
    float m_q = 0.70710678f;      ///< clamped to [0.025, 40]
    bool m_enabled = true;        ///< a disabled band is flat

    friend bool operator==(const EqBand&, const EqBand&) = default;
};

/**
 * @brief A parametric equaliser of up to k_max_bands bands, for any number of channels.
 *
 * Each band is a trapezoidal state-variable filter (Cytomic's linear trapezoidal SVF) per
 * channel, its output a mix of the filter's taps, so every band type has the analogue
 * prototype's response, bilinear-transformed with the band's frequency prewarped: the same
 * magnitude as the RBJ cookbook biquads (Web Audio's BiquadFilterNode). Like them, peaks and
 * shelves narrow towards Nyquist (cramping). The bands run in series.
 *
 * Every change of a band (frequency, gain, Q, type, on or off) ramps its coefficients over
 * k_ramp_ms, so automation and a dragged band do not click; a band disabled (or flat) once its
 * ramp ends costs nothing. magnitude_db() and response_db() give the response the bands settle
 * to, for drawing the curve.
 *
 * Threads: prepare() allocates (audio stopped). set_band(), process() and reset() are real-time
 * safe and belong to the audio thread. magnitude_db() and response_db() are pure, any thread.
 * Denormals are left to the host's flush-to-zero (as for every filter here).
 */
class TANH_API ParametricEq {
public:
    static constexpr size_t k_max_bands = 8;
    static constexpr double k_ramp_ms = 10.0;

    /// Allocate the state for @p num_channels channels and set every band flat (disabled),
    /// with no ramp.
    void prepare(double sample_rate, size_t num_channels);

    /// Set band @p index (< k_max_bands); a band that changed ramps to its new coefficients.
    /// Cheap when nothing changed, so a caller may set every band every block.
    void set_band(size_t index, const EqBand& band) TANH_NONBLOCKING_FUNCTION;
    [[nodiscard]] const EqBand& band(size_t index) const { return m_bands[index].m_settings; }

    /// Filter @p num_samples of the first @p num_channels (<= the prepared count) channels,
    /// in place.
    void process(float* const* channels,
                 size_t num_channels,
                 size_t num_samples) TANH_NONBLOCKING_FUNCTION;

    /// Clear the filters' state and end every ramp at its target.
    void reset() TANH_NONBLOCKING_FUNCTION;

    /// The gain in dB at @p frequency of @p band settled, at @p sample_rate.
    [[nodiscard]] static float magnitude_db(const EqBand& band,
                                            double frequency,
                                            double sample_rate);
    /// The gain in dB at @p frequency of @p bands in series, settled.
    [[nodiscard]] static float response_db(std::span<const EqBand> bands,
                                           double frequency,
                                           double sample_rate);

private:
    /// Cytomic's SVF: integrator gain g, damping k, output m0 * in + m1 * band + m2 * low.
    struct Coefficients {
        float m_g = 0.0f;
        float m_k = 1.0f;
        float m_m0 = 1.0f;
        float m_m1 = 0.0f;
        float m_m2 = 0.0f;
    };

    struct Band {
        EqBand m_settings;
        Coefficients m_current;
        Coefficients m_target;
        Coefficients m_step;
        uint32_t m_ramp_remaining = 0;
        bool m_flat = true;  // the target is the identity: skipped once the ramp ends
    };

    [[nodiscard]] static Coefficients coefficients_of(const EqBand& band, double sample_rate);
    void process_band(size_t index, float* const* channels, size_t num_channels, size_t num_samples)
        TANH_NONBLOCKING_FUNCTION;

    double m_sample_rate = 48000.0;
    size_t m_num_channels = 0;
    uint32_t m_ramp_samples = 480;
    std::array<Band, k_max_bands> m_bands{};
    std::vector<float> m_state;  // per band and channel: the two integrator states
};

}  // namespace thl::dsp::filter
