#pragma once

#include <tanh/core/Exports.h>
#include <tanh/dsp/filter/SvfCoefficients.h>

#include <complex>
#include <cstdint>

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

/// One band of an equaliser. Out-of-range or non-finite values are clamped or replaced.
struct EqBand {
    EqBandType m_type = EqBandType::Peak;
    float m_frequency = 1000.0f;  ///< Hz, clamped to [10, 0.49 * sample rate]
    float m_gain_db = 0.0f;       ///< Peak and the shelves, clamped to [-48, 48]
    float m_q = 0.70710678f;      ///< clamped to [0.025, 40]
    bool m_enabled = true;        ///< a disabled band is flat

    friend bool operator==(const EqBand&, const EqBand&) = default;
};

/// How a band's analogue prototype becomes a digital filter.
enum class EqDesign : uint8_t {
    /// Bilinear transform with the frequency prewarped: the RBJ cookbook's curves. Exact at
    /// the band's frequency; peaks and shelves narrow (cramp) towards Nyquist.
    Bilinear,
    /// Magnitude matched to the analogue prototype up to Nyquist: poles by impulse invariance,
    /// zeros fitted to the prototype's magnitude: Vicanek 2016 for the peak and the cuts; the
    /// shelves fitted at DC, the band's frequency and Nyquist, with the poles of the shelf whose
    /// poles are the lower pair, up to Q 2 (within 0.2 dB of the bilinear shelf's error there,
    /// mostly far better; above Q 2 the fit misses a resonant shelf's bump, so those shelves are
    /// bilinear). The notch stays bilinear, which keeps its zero exactly at its frequency. Falls
    /// back to Bilinear where a fit does not exist.
    Matched,
};

/// @p band with its values in range: non-finite values become the defaults, the rest clamped.
[[nodiscard]] TANH_API EqBand sanitised(const EqBand& band,
                                        double sample_rate) TANH_NONBLOCKING_FUNCTION;

/// True when @p band passes its input unchanged (disabled, or a peak or shelf at 0 dB).
[[nodiscard]] TANH_API bool is_flat(const EqBand& band) TANH_NONBLOCKING_FUNCTION;

/// The digital filter of @p band at @p sample_rate. Real-time safe (no allocation).
[[nodiscard]] TANH_API SvfCoefficients design_band(const EqBand& band,
                                                   EqDesign design,
                                                   double sample_rate) TANH_NONBLOCKING_FUNCTION;

/// The analogue prototype's response at @p frequency: what a design aims for.
[[nodiscard]] TANH_API std::complex<double> analog_response(const EqBand& band, double frequency);

}  // namespace thl::dsp::filter
