#pragma once

#include <tanh/dsp/granular/GranularTypes.h>

#include <algorithm>
#include <cstddef>

namespace thl::dsp::granular {

// The part of a bank a head may play, in source frames, plus its
// direction. Start is the entry, End the exit, Loop the re-entry — so End
// before Start plays backwards. The bounds are always stored ascending
// ([m_start, m_end) with m_loop_point inside) and the head logic runs in
// those "virtual" forward coordinates; a reversed region only mirrors the
// position that is finally read: physical = m_start + m_end - 1 - virtual.
// Resolved once per block (GrainEngine::render via the HeadPolicy,
// SamplePlayer::render) — that resolution is the seam where a slice source
// (slicer mode) plugs in: a slice replaces Start / End / Loop with the
// slice's bounds and nothing below this struct needs to know.
struct SampleRegion {
    size_t m_start{0};       // lower bound, virtual entry
    size_t m_end{0};         // upper bound (exclusive), virtual exit
    size_t m_loop_point{0};  // virtual re-entry, inside [m_start, m_end]
    bool m_reverse{false};

    size_t size() const { return m_end - m_start; }

    // Virtual -> physical read position.
    double physical(double virtual_position) const {
        if (!m_reverse) { return virtual_position; }
        return static_cast<double>(m_start + m_end - 1) - virtual_position;
    }
    FramePos physical(FramePos virtual_position) const {
        if (!m_reverse) { return virtual_position; }
        return static_cast<FramePos>(m_start + m_end - 1) - virtual_position;
    }

    static SampleRegion full(size_t total_frames) {
        return {.m_start = 0, .m_end = total_frames, .m_loop_point = 0, .m_reverse = false};
    }

    // Normalised [0, 1] markers to frames. Inputs are clamped before the
    // size_t casts: a negative float -> size_t cast is UB (on x86 it wraps,
    // collapsing the region to zero and muting the voice). End below Start
    // reverses; the loop point is mirrored into virtual coordinates with
    // the bounds.
    static SampleRegion from_normalized(float start, float end, float loop, size_t total_frames) {
        auto const total_f = static_cast<float>(total_frames);
        auto s = static_cast<size_t>(std::clamp(start, 0.0f, 1.0f) * total_f);
        auto e = static_cast<size_t>(std::clamp(end, 0.0f, 1.0f) * total_f);
        auto l = static_cast<size_t>(std::clamp(loop, 0.0f, 1.0f) * total_f);
        s = std::min(s, total_frames);
        e = std::min(e, total_frames);
        bool const reverse = e < s;
        size_t const lo = std::min(s, e);
        size_t const hi = std::max(s, e);
        l = std::clamp(l, lo, hi);
        if (reverse && hi > lo) { l = lo + hi - std::min(l, hi - 1) - 1; }
        return {.m_start = lo, .m_end = hi, .m_loop_point = l, .m_reverse = reverse};
    }
};

}  // namespace thl::dsp::granular
