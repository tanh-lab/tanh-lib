#include "tanh/modulation/MotionLaneProcessing.h"

#include <tanh/modulation/MotionLane.h>
#include <tanh/modulation/detail/MotionSeam.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace thl::modulation {

namespace {

// Smoothing sigma at amount 1, in lane units (half a beat, or 250 ms).
constexpr double k_max_sigma_beats = 0.5;
constexpr double k_max_sigma_seconds = 0.25;
// Below this many points of sigma the kernel is a no-op.
constexpr double k_min_smoothing_points = 0.05;
// Sigma is capped at this fraction of the loop: a short Seconds lane at a large
// amount flattens towards its mean instead of wrapping the kernel many times.
constexpr double k_max_sigma_fraction = 1.0 / 6.0;

// Convolution of the loop with a sampled Gaussian, in recorded order from
// @p seam. Periodic: the kernel wraps around the loop, so the seam is smoothed
// like any other point. Otherwise both ends are mirrored (half-sample
// reflection) and the edge at the seam stays sharp. The kernel is symmetric
// (zero phase) and its weights sum to 1: the output stays inside [0, 1].
std::vector<float> smooth_gaussian(const std::vector<float>& in,
                                   double sigma,
                                   size_t seam,
                                   bool periodic) {
    const size_t n = in.size();
    std::vector<float> out(n, 0.0f);
    if (n == 0) { return out; }
    // Truncate at 3 sigma (sigma <= n / 6 keeps the kernel within one loop).
    const auto radius =
        static_cast<std::ptrdiff_t>(std::min(std::ceil(3.0 * sigma), static_cast<double>(n)));
    std::vector<double> weights(static_cast<size_t>((2 * radius) + 1));
    double sum = 0.0;
    for (std::ptrdiff_t k = -radius; k <= radius; ++k) {
        const auto d = static_cast<double>(k);
        const double w = std::exp(-0.5 * d * d / (sigma * sigma));
        weights[static_cast<size_t>(k + radius)] = w;
        sum += w;
    }
    const auto len = static_cast<std::ptrdiff_t>(n);
    auto source = [&](std::ptrdiff_t j) {
        if (periodic) {
            j %= len;
            if (j < 0) { j += len; }
        } else {
            while (j < 0 || j >= len) { j = j < 0 ? -j - 1 : (2 * len) - 1 - j; }
        }
        return in[(seam + static_cast<size_t>(j)) % n];
    };
    for (std::ptrdiff_t i = 0; i < len; ++i) {
        double acc = 0.0;
        for (std::ptrdiff_t k = -radius; k <= radius; ++k) {
            acc += weights[static_cast<size_t>(k + radius)] * static_cast<double>(source(i + k));
        }
        out[(seam + static_cast<size_t>(i)) % n] =
            static_cast<float>(std::clamp(acc / sum, 0.0, 1.0));
    }
    return out;
}

}  // namespace

PlayedCurve make_played_curve(const MotionLane& lane, float smoothing, LoopEnd loop_end) {
    const double amount = std::clamp(static_cast<double>(smoothing), 0.0, 1.0);
    const bool jump = loop_end == LoopEnd::Jump;
    const size_t n = lane.num_points();
    const size_t seam = lane.m_seam < n ? lane.m_seam : 0;
    PlayedCurve curve{.m_x = lane.m_x, .m_y = lane.m_y, .m_smoothed = amount > 0.0 || jump};
    if (n < detail::k_motion_min_points || !(lane.m_length > 0.0)) { return curve; }

    if (!jump) {
        // Close the seam over a window from the base (amount 0, as a take buffer
        // plays) to the whole loop (amount 1, a smooth detrend).
        const size_t base = detail::motion_seam_window(n, lane.m_length, lane.m_timebase);
        const size_t window =
            base + static_cast<size_t>(std::round(amount * static_cast<double>(n - base)));
        for (auto* p : {&curve.m_x, &curve.m_y}) {
            auto at = [&](size_t i) -> float& { return (*p)[(seam + i) % n]; };
            detail::motion_spread_seam(at, n, window, detail::motion_seam_error(at, n));
        }
    }
    const double max_sigma =
        lane.m_timebase == MotionTimebase::Beats ? k_max_sigma_beats : k_max_sigma_seconds;
    const double points_per_unit = static_cast<double>(n) / lane.m_length;
    const double sigma = std::min(max_sigma * amount * amount * points_per_unit,
                                  k_max_sigma_fraction * static_cast<double>(n));
    if (sigma >= k_min_smoothing_points) {
        curve.m_x = smooth_gaussian(curve.m_x, sigma, seam, !jump);
        curve.m_y = smooth_gaussian(curve.m_y, sigma, seam, !jump);
    }
    return curve;
}

}  // namespace thl::modulation
