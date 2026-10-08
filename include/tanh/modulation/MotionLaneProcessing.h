#pragma once

#include <tanh/core/Exports.h>
#include <tanh/modulation/MotionLane.h>

#include <cstdint>
#include <vector>

namespace thl::modulation {

/// How a played lane treats the step from the end of the loop to its start.
enum class LoopEnd : uint8_t {
    Smooth,  ///< spread the step over a window that grows with the smoothing amount (default)
    Jump     ///< keep the step as a hard edge; smoothing does not cross it
};

/// The x and y a lane plays, after make_played_curve().
struct PlayedCurve {
    std::vector<float> m_x;
    std::vector<float> m_y;
    /// Differs from the raw lane (smoothing > 0, or LoopEnd::Jump).
    bool m_smoothed = false;
};

/**
 * @brief The curve @p lane plays with @p smoothing (0 to 1) and @p loop_end; the lane stays raw.
 *
 * LoopEnd::Smooth spreads the step at the seam (MotionLane::m_seam) over a window
 * that grows from 150 ms (0.3 beat) at 0 to the whole loop at 1. Then x and y go
 * through a zero-phase Gaussian of sigma = smoothing² × half a beat (250 ms for
 * Seconds lanes), at most a sixth of the loop: circular for Smooth, mirrored at
 * the seam for Jump. Values stay in [0, 1]. Allocates (message thread).
 */
[[nodiscard]] TANH_API PlayedCurve make_played_curve(const MotionLane& lane,
                                                     float smoothing,
                                                     LoopEnd loop_end);

}  // namespace thl::modulation
