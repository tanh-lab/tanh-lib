#pragma once

#include <tanh/core/Numbers.h>
#include <tanh/dsp/sampler/LoopRegion.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace thl::dsp::sampler {

/**
 * @brief Quarter-sine crossfade table.
 *
 * `at(k) = sin(k / length * pi / 2)` for integer k in [0, length]. Because
 * `cos(t * pi/2) == sin((1 - t) * pi/2)`, one table serves the fade in and
 * the fade out exactly: no interpolation and no sin/cos on the audio thread.
 *
 * @par Real-Time Safety
 * prepare() allocates; every other member is real-time safe.
 */
class FadeCurve {
public:
    /// Build the table for a fade of `length` steps (at least 1).
    void prepare(size_t length) {
        m_length = std::max<size_t>(1, length);
        m_curve.resize(m_length + 1);
        for (size_t k = 0; k <= m_length; ++k) {
            float const t = static_cast<float>(k) / static_cast<float>(m_length);
            m_curve[k] = std::sin(t * std::numbers::pi_v<float> * 0.5f);
        }
    }

    [[nodiscard]] size_t length() const { return m_length; }

    /// Gain at fade position t in [0, 1] (nearest step): 0 -> 0, 1 -> 1.
    [[nodiscard]] float at(double t) const {
        auto const index = static_cast<size_t>(
            std::lround(std::clamp(t, 0.0, 1.0) * static_cast<double>(m_length)));
        return m_curve[std::min(index, m_length)];
    }

    /// Fade-in gain with `remaining` of length() steps left (0 left = 1).
    [[nodiscard]] float in_gain(size_t remaining) const {
        if (remaining == 0) { return 1.0f; }
        return m_curve[m_length - std::min(remaining, m_length)];
    }

    /// Fade-out gain with `remaining` of length() steps left.
    [[nodiscard]] float out_gain(size_t remaining) const {
        return m_curve[std::min(remaining, m_length)];
    }

private:
    size_t m_length{1};
    std::vector<float> m_curve{0.0f, 1.0f};
};

/**
 * @brief Where a loop wrap fades, in source frames.
 *
 * A loop fade blends the head into a copy reading the other side of the
 * jump, so the jump itself is silent:
 * - `m_pre`: over the last frames before End, the copy reads the audio just
 *   before Loop and fades in; at End the head is already reading what
 *   follows Loop (a hardware sampler's loop crossfade).
 * - `m_post`: after the wrap, the head starts at Loop under a copy that
 *   carries on past End and fades out (when there is no room before Loop).
 *
 * At most one is non-zero; both zero means no room to fade.
 */
struct LoopFadePlan {
    double m_point{0.0};  ///< Effective (virtual) re-entry.
    double m_pre{0.0};
    double m_post{0.0};
};

/// Loop marker with its body floored to `min_body` frames (a region shorter
/// than that loops whole), so the loop fade cannot shrink to nothing.
inline double floored_loop_point(const LoopRegion& region, double min_body) {
    auto const end = static_cast<double>(region.m_end);
    double const floor = std::min(static_cast<double>(region.size()), min_body);
    return std::min(static_cast<double>(region.m_loop_point), end - floor);
}

/**
 * @brief Plan the loop fade for a region.
 *
 * @param region      The region looping.
 * @param total_frames Frames of the sample (bounds the copy's reads).
 * @param max_fade    Longest fade wanted, in source frames (fade time times
 *                    playback rate).
 * @param min_body    Minimum loop body, see floored_loop_point().
 *
 * Each fade is at most half the loop body and no longer than the audio the
 * copy reads. Reversed, virtual "before Loop" is the sample above the
 * region's top and "past End" the sample below its bottom. The side with
 * more room wins, before End on a tie; under one frame there is no fade.
 */
inline LoopFadePlan plan_loop_fade(const LoopRegion& region,
                                   size_t total_frames,
                                   double max_fade,
                                   double min_body) {
    LoopFadePlan plan;
    plan.m_point = floored_loop_point(region, min_body);
    auto const lo = static_cast<double>(region.m_start);
    auto const hi = static_cast<double>(region.m_end);
    auto const total = static_cast<double>(total_frames);
    double const cap = std::min(max_fade, 0.5 * (hi - plan.m_point));
    double const before_loop = region.m_reverse ? plan.m_point + total - lo - hi : plan.m_point;
    double const past_end = region.m_reverse ? lo : total - hi;
    double const pre = std::min(cap, before_loop);
    double const post = std::min(cap, past_end);
    if (pre >= post) {
        plan.m_pre = pre >= 1.0 ? pre : 0.0;
    } else {
        plan.m_post = post >= 1.0 ? post : 0.0;
    }
    return plan;
}

}  // namespace thl::dsp::sampler
