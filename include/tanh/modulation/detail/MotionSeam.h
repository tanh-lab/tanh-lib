#pragma once

// Closing the step at a loop's seam, shared by MotionRecorder (in place on a
// take buffer, real-time) and make_played_curve() (on a published lane).

#include <tanh/modulation/MotionLane.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>

namespace thl::modulation::detail {

/// Fewer points than this are not a loop (no seam closing, no smoothing; a shorter take is
/// dropped).
inline constexpr size_t k_motion_min_points = 4;

/// Seam window at smoothing 0 in points: 150 ms or 0.3 beat (150 ms at 120 BPM), at least
/// one point and at most a quarter of the loop.
[[nodiscard]] inline size_t motion_seam_window(size_t n, double length, MotionTimebase timebase) {
    constexpr double k_window_seconds = 0.15;
    constexpr double k_window_beats = 0.3;
    const double units = timebase == MotionTimebase::Beats ? k_window_beats : k_window_seconds;
    const double points_per_unit = static_cast<double>(n) / length;
    return static_cast<size_t>(std::max(
        1.0,
        std::min(std::floor(static_cast<double>(n) / 4.0), std::round(units * points_per_unit))));
}

/// The step from the last point to the first beyond the expected step, which is
/// the mean velocity on both sides (three points each, robust against a single
/// jittery point). at(i) is the i-th point in recorded order, n >= k_motion_min_points.
template <typename At>
[[nodiscard]] double motion_seam_error(At at, size_t n) {
    const double p0 = at(0);
    const double pn = at(n - 1);
    const double vel = n >= 8 ? (((at(3) - p0) / 3.0) + ((pn - at(n - 4)) / 3.0)) * 0.5
                              : static_cast<double>(at(1)) - p0;
    return (p0 - pn) - vel;
}

/// Adds @p error to the last @p window points with a raised-cosine ramp (the last
/// point moves by the whole error, the point before the window not at all),
/// clamped to [0, 1].
template <typename At>
void motion_spread_seam(At at, size_t n, size_t window, double error) {
    for (size_t j = 0; j < window; ++j) {
        const double w = 0.5 * (1.0 - std::cos(std::numbers::pi * static_cast<double>(j + 1) /
                                               static_cast<double>(window)));
        float& v = at(n - window + j);
        v = static_cast<float>(std::clamp(static_cast<double>(v) + (error * w), 0.0, 1.0));
    }
}

}  // namespace thl::modulation::detail
