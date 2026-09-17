#pragma once

#include <tanh/dsp/granular/GranularTypes.h>
#include <tanh/dsp/granular/SliceMap.h>

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
        return from_frames(to_frame(start, total_frames),
                           to_frame(end, total_frames),
                           to_frame(loop, total_frames),
                           total_frames);
    }

    // One normalised marker to its frame, as from_normalized reads it.
    static size_t to_frame(float normalized, size_t total_frames) {
        return static_cast<size_t>(std::clamp(normalized, 0.0f, 1.0f) *
                                   static_cast<float>(total_frames));
    }

    // Start / End / Loop already in frames (e.g. snapped by the Sample head).
    static SampleRegion from_frames(size_t s, size_t e, size_t l, size_t total_frames) {
        s = std::min(s, total_frames);
        e = std::min(e, total_frames);
        bool const reverse = e < s;
        size_t const lo = std::min(s, e);
        size_t const hi = std::max(s, e);
        return {.m_start = lo,
                .m_end = hi,
                .m_loop_point = virtual_loop(l, lo, hi, reverse),
                .m_reverse = reverse};
    }

    // Loop (physical frame) as the virtual re-entry. Outside the region, or
    // on its exit (the last frame before End — for a reversed region that is
    // the lowest frame), there is no loop body to play: the region loops
    // whole from its entry. The UI clamps Loop into [Start, End], so
    // squeezing the markers together parks Loop on either one — including
    // End, which used to leave a one-frame body (a 2 ms buzz once floored).
    static size_t virtual_loop(size_t l, size_t lo, size_t hi, bool reverse) {
        if (hi <= lo || l < lo || l >= hi) { return lo; }
        size_t const v = reverse ? lo + hi - 1 - l : l;
        return v + 1 >= hi ? lo : v;
    }

    // Slicing: Start and End are read in slice space and snap to
    // boundaries; the slices between them play. Start and End on the same
    // boundary play that one slice (the next one, or the last one at the
    // final boundary). End before Start reverses. There is no Loop marker:
    // the region re-enters at its own start (Start -> End repeats).
    static SampleRegion from_slices(float start,
                                    float end,
                                    const SliceMap& map,
                                    size_t total_frames) {
        int s = map.boundary_of_step(start);
        int e = map.boundary_of_step(end);
        if (s == e) {
            if (s < map.m_count) {
                e = s + 1;
            } else {
                s = e - 1;
            }
        }
        bool const reverse = e < s;
        size_t const lo = map.boundary_frame(std::min(s, e), total_frames);
        size_t const hi = map.boundary_frame(std::max(s, e), total_frames);
        return {.m_start = lo, .m_end = hi, .m_loop_point = lo, .m_reverse = reverse};
    }
};

}  // namespace thl::dsp::granular
