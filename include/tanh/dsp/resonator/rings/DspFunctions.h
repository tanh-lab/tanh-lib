// Inline replacements for the generated LUT data in the original Rings
// Resources.cpp.  Three analytic functions replace interpolated tables;
// two computed-once tables replace the sine wavetable and FM frequency
// quantiser.  See src/dsp/resonator/rings/lookup_tables.py for the
// original Python definitions.
//
// Original LUT code: Copyright 2015 Emilie Gillet (MIT licence).

#ifndef TANH_DSP_RESONATOR_RINGS_DSP_FUNCTIONS_H_
#define TANH_DSP_RESONATOR_RINGS_DSP_FUNCTIONS_H_

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace thl::dsp::resonator::rings {

constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kOneOverPi = std::numbers::inv_pi_v<float>;

// Replaces Interpolate(lut_stiffness, structure, 256.0f).
// Piecewise mapping from structure [0,1] to inharmonicity coefficient.
// The original LUT forces entries 255-256 to 2.0; this function yields
// ~1.985 at structure ~0.996 instead -- a negligible difference.
inline float Stiffness(float structure) {
    if (structure < 0.25f) {
        return -(0.25f - structure) * 0.25f;
    } else if (structure < 0.3f) {
        return 0.0f;
    } else if (structure < 0.9f) {
        float g = (structure - 0.3f) / 0.6f;
        return 0.01f * std::pow(10.0f, g * 2.005f) - 0.01f;
    } else {
        float g = (structure - 0.9f) * 10.0f;
        g *= g;
        return std::min(1.5f - std::cos(g * kPi) * 0.5f, 2.0f);
    }
}

// Replaces Interpolate(lut_4_decades, x, 256.0f).
// Maps [0,1] -> [1, 10000] on a logarithmic scale.
inline float FourDecades(float x) {
    return std::pow(10.0f, 4.0f * x);
}

// Replaces Interpolate(lut_svf_shift, damping_cutoff, 1.0f).
// SVF delay compensation: cutoff_semitones is used as a direct table
// index (size=1.0f in the original Interpolate call).
inline float SvfShift(float cutoff_semitones) {
    float ratio = std::exp2(cutoff_semitones / 12.0f);
    return std::atan(1.0f / ratio) * kOneOverPi;
}

namespace detail {

inline std::array<float, 5121> MakeSineTable() {
    std::array<float, 5121> t;
    constexpr double pi2 = 2.0 * std::numbers::pi;
    for (size_t i = 0; i < t.size(); ++i) {
        t[i] = static_cast<float>(
            std::sin(pi2 * static_cast<double>(i) / 4096.0));
    }
    return t;
}

inline std::array<float, 129> MakeFmFrequencyQuantizerTable() {
    const double kDetune16Cents = std::exp2(16.0 / 1200.0);
    const double kRatios[] = {
        0.5,
        0.5 * kDetune16Cents,
        0.7071067811865476,         // sqrt(2)/2
        0.7853981633974483,         // pi/4
        1.0,
        1.0 * kDetune16Cents,
        1.4142135623730951,         // sqrt(2)
        1.5707963267948966,         // pi/2
        1.75,                       // 7/4
        2.0,
        2.0 * kDetune16Cents,
        2.25,                       // 9/4
        2.75,                       // 11/4
        2.8284271247461903,         // 2*sqrt(2)
        3.0,
        3.141592653589793,          // pi
        3.4641016151377544,         // sqrt(3)*2
        4.0,
        4.242640687119285,          // sqrt(2)*3
        4.71238898038469,           // pi*3/2
        5.0,
        5.656854249492381,          // sqrt(2)*4
        8.0,
    };

    std::array<double, 129> scale {};
    size_t len = 0;
    for (double r : kRatios) {
        double semitones = 12.0 * std::log2(r);
        scale[len++] = semitones;
        scale[len++] = semitones;
        scale[len++] = semitones;
    }

    size_t target = 1;
    while (target < len) {
        target <<= 1;
    }

    while (len < target) {
        size_t max_index = 0;
        double max_diff = -1.0;
        for (size_t i = 0; i + 1 < len; ++i) {
            double diff = scale[i + 1] - scale[i];
            if (diff > max_diff) {
                max_diff = diff;
                max_index = i;
            }
        }

        const double mid = 0.5 * (scale[max_index] + scale[max_index + 1]);
        for (size_t i = len; i > max_index + 1; --i) {
            scale[i] = scale[i - 1];
        }
        scale[max_index + 1] = mid;
        ++len;
    }

    scale[len] = scale[len - 1];
    ++len;

    std::array<float, 129> result {};
    for (size_t i = 0; i < result.size(); ++i) {
        result[i] = static_cast<float>(scale[i]);
    }
    return result;
}

}  // namespace detail

// Replaces lut_sine[].  4096 + 1024 + 1 = 5121 entries covering one
// full period plus an extra quarter for safe interpolation overshoot.
// Built on first use unless pre-warmed via WarmDspFunctions() during Init().
inline const std::array<float, 5121>& SineTableStorage() {
    static const std::array<float, 5121> table = detail::MakeSineTable();
    return table;
}

// Replaces lut_fm_frequency_quantizer[]. 129 entries built from 23
// musically interesting FM frequency ratios (see lookup_tables.py).
// Built on first use unless pre-warmed via WarmDspFunctions() during Init().
inline const std::array<float, 129>& FmFrequencyQuantizerTableStorage() {
    static const std::array<float, 129> table =
        detail::MakeFmFrequencyQuantizerTable();
    return table;
}

// Call from non-audio-thread Init() paths to ensure table construction never
// occurs during audio processing.
inline void WarmDspFunctions() {
    (void) SineTableStorage();
    (void) FmFrequencyQuantizerTableStorage();
}

inline const float* SineTable() {
    return SineTableStorage().data();
}

inline const float* FmFrequencyQuantizerTable() {
    return FmFrequencyQuantizerTableStorage().data();
}

}  // namespace thl::dsp::resonator::rings

#endif  // TANH_DSP_RESONATOR_RINGS_DSP_FUNCTIONS_H_
