#pragma once

#include <tanh/dsp/sampler/SampleView.h>

#include <algorithm>
#include <cstddef>

namespace thl::dsp::sampler {

/**
 * @brief The part of a sample a play head may play, plus its direction.
 *
 * Start is the entry, End the exit and Loop the re-entry, so End before
 * Start plays backwards. The bounds are always stored ascending
 * (`[m_start, m_end)` with `m_loop_point` inside) and head logic runs in
 * those "virtual" forward coordinates; a reversed region only mirrors the
 * position that is finally read: `physical = m_start + m_end - 1 - virtual`.
 *
 * Anything that decides what plays — markers, a slice, a host's own logic —
 * resolves to a LoopRegion; nothing below it needs to know which.
 */
struct LoopRegion {
    size_t m_start{0};       ///< Lower bound, virtual entry.
    size_t m_end{0};         ///< Upper bound (exclusive), virtual exit.
    size_t m_loop_point{0};  ///< Virtual re-entry, inside [m_start, m_end].
    bool m_reverse{false};   ///< Plays from the top down.

    [[nodiscard]] size_t size() const { return m_end - m_start; }

    /// Virtual -> physical read position.
    [[nodiscard]] double physical(double virtual_position) const {
        if (!m_reverse) { return virtual_position; }
        return static_cast<double>(m_start + m_end - 1) - virtual_position;
    }
    /// Virtual -> physical read position (integer frames).
    [[nodiscard]] FramePos physical(FramePos virtual_position) const {
        if (!m_reverse) { return virtual_position; }
        return static_cast<FramePos>(m_start + m_end - 1) - virtual_position;
    }

    /// The whole sample, forwards, looping from its start.
    static LoopRegion full(size_t total_frames) {
        return {.m_start = 0, .m_end = total_frames, .m_loop_point = 0, .m_reverse = false};
    }

    /**
     * @brief Normalised [0, 1] markers to a region.
     *
     * Inputs are clamped before the size_t casts (a negative float -> size_t
     * cast is undefined). End below Start reverses; Loop is mirrored into
     * virtual coordinates with the bounds (see virtual_loop()).
     */
    static LoopRegion from_normalized(float start, float end, float loop, size_t total_frames) {
        return from_frames(to_frame(start, total_frames),
                           to_frame(end, total_frames),
                           to_frame(loop, total_frames),
                           total_frames);
    }

    /// One normalised marker to its frame, as from_normalized() reads it.
    static size_t to_frame(float normalized, size_t total_frames) {
        return static_cast<size_t>(std::clamp(normalized, 0.0f, 1.0f) *
                                   static_cast<float>(total_frames));
    }

    /// Start / End / Loop already in (physical) frames.
    static LoopRegion from_frames(size_t start, size_t end, size_t loop, size_t total_frames) {
        start = std::min(start, total_frames);
        end = std::min(end, total_frames);
        bool const reverse = end < start;
        size_t const lo = std::min(start, end);
        size_t const hi = std::max(start, end);
        return {.m_start = lo,
                .m_end = hi,
                .m_loop_point = virtual_loop(loop, lo, hi, reverse),
                .m_reverse = reverse};
    }

    /**
     * @brief Loop (a physical frame) as the virtual re-entry.
     *
     * Outside the region, or on its exit (the last frame before End; for a
     * reversed region that is the lowest frame), there is no loop body to
     * play, so the region loops whole from its entry. A UI that clamps Loop
     * into [Start, End] parks it on End when the markers are squeezed
     * together; that must not leave a one-frame loop.
     */
    static size_t virtual_loop(size_t loop, size_t lo, size_t hi, bool reverse) {
        if (hi <= lo || loop < lo || loop >= hi) { return lo; }
        size_t const v = reverse ? lo + hi - 1 - loop : loop;
        return v + 1 >= hi ? lo : v;
    }
};

}  // namespace thl::dsp::sampler
