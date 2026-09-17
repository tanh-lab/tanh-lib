#pragma once

#include <tanh/dsp/sampler/SampleView.h>

#include <algorithm>
#include <cstddef>

namespace thl::dsp::sampler {

/**
 * @brief True when `frame` is an upward zero crossing of the view.
 *
 * Judged on the sum of the first two channels (one for mono): the frame
 * before is negative and this one is not. Snapping both ends of a loop to
 * crossings of the same direction is what makes the join like to like.
 */
inline bool upward_crossing_at(const SampleView& view, size_t frame) {
    if (!view.valid() || frame == 0 || frame >= view.m_num_frames) { return false; }
    const float* const a = view.m_channels[0];
    const float* const b = view.m_num_channels > 1 ? view.m_channels[1] : nullptr;
    float const before = b != nullptr ? a[frame - 1] + b[frame - 1] : a[frame - 1];
    float const at = b != nullptr ? a[frame] + b[frame] : a[frame];
    return before < 0.0f && at >= 0.0f;
}

/**
 * @brief The upward zero crossing nearest to `target` within `radius`
 * frames, inside [lo, hi].
 *
 * Searches nearest first, alternating sides (at most 2 * radius + 1 checks).
 * With no crossing in reach it returns `target` clamped to [lo, hi]; check
 * upward_crossing_at() on the result to tell the two apart.
 *
 * @par Real-Time Safety
 * Real-time safe, but its cost grows with `radius`: cache the result while
 * the target does not move.
 */
inline size_t nearest_upward_crossing(const SampleView& view,
                                      size_t target,
                                      size_t lo,
                                      size_t hi,
                                      size_t radius) {
    target = std::clamp(target, lo, hi);
    for (size_t d = 0; d <= radius; ++d) {
        bool const up_in = target + d <= hi;
        bool const down_in = target >= lo + d;
        if (!up_in && !down_in) { break; }
        if (up_in && upward_crossing_at(view, target + d)) { return target + d; }
        if (d > 0 && down_in && upward_crossing_at(view, target - d)) { return target - d; }
    }
    return target;
}

}  // namespace thl::dsp::sampler
