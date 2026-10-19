#pragma once

// Phase wrapping and looped Catmull-Rom of motion lanes (private to tanh::Modulation).

#include <cmath>
#include <cstddef>

namespace thl::modulation::detail {

/// Wrap @p value into [0, length). Returns 0 for length <= 0 or a non-finite value.
[[nodiscard]] inline double wrap_phase(double value, double length) {
    if (!(length > 0.0) || !std::isfinite(value)) { return 0.0; }
    double p = value - (std::floor(value / length) * length);
    if (p >= length || p < 0.0) { p = 0.0; }
    return p;
}

/// Uniform Catmull-Rom through @p n looping points at the fractional @p index,
/// clamped to [0, 1]. Returns 0 for n == 0.
[[nodiscard]] float catmull_rom_wrap(const float* points, size_t n, double index);

}  // namespace thl::modulation::detail
