#pragma once

#include <tanh/core/AbiNamespace.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <cmath>

namespace thl::inline THL_ABI_NAMESPACE::dsp::utils {

/// @p ms milliseconds in samples at @p sample_rate (not rounded).
[[nodiscard]] inline double ms_to_samples(double ms, double sample_rate) TANH_NONBLOCKING_FUNCTION {
    return ms * 0.001 * sample_rate;
}

/**
 * The pole of a one-pole lowpass with the time constant @p samples: exp(-1 / samples), so that
 * y = x + pole · (y - x) covers 1 - 1/e (63 %) of a step after @p samples. 0 (no smoothing) for
 * no time. The coefficient of dsp::utils::one_pole() (DspMath.h) is 1 - pole.
 */
[[nodiscard]] inline float one_pole_pole(double samples) TANH_NONBLOCKING_FUNCTION {
    return samples > 0.0 ? static_cast<float>(std::exp(-1.0 / samples)) : 0.0f;
}

/// one_pole_pole() for a time constant of @p ms milliseconds at @p sample_rate.
[[nodiscard]] inline float one_pole_pole(double ms, double sample_rate) TANH_NONBLOCKING_FUNCTION {
    return one_pole_pole(ms_to_samples(ms, sample_rate));
}

}  // namespace thl::inline THL_ABI_NAMESPACE::dsp::utils
