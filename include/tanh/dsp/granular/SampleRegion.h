#pragma once

#include <algorithm>
#include <cstddef>

namespace thl::dsp::granular {

// The part of a bank a head may play, in source frames. Resolved once per
// block by the active HeadPolicy — that resolution is the seam where a
// slice source (slicer mode) plugs in: a slice replaces Start / End / Loop
// with the slice's bounds and nothing below this struct needs to know.
struct SampleRegion {
    size_t m_start{0};
    size_t m_end{0};
    size_t m_loop_point{0};

    size_t size() const { return m_end - m_start; }

    static SampleRegion full(size_t total_frames) {
        return {.m_start = 0, .m_end = total_frames, .m_loop_point = 0};
    }

    // Normalised [0, 1] markers to frames. Inputs are clamped before the
    // size_t casts: a negative float -> size_t cast is UB (on x86 it wraps,
    // collapsing the region to zero and muting the voice).
    static SampleRegion from_normalized(float start, float end, float loop, size_t total_frames) {
        auto const total_f = static_cast<float>(total_frames);
        auto s = static_cast<size_t>(std::clamp(start, 0.0f, 1.0f) * total_f);
        auto e = static_cast<size_t>(std::clamp(end, 0.0f, 1.0f) * total_f);
        auto l = static_cast<size_t>(std::clamp(loop, 0.0f, 1.0f) * total_f);
        s = std::min(s, total_frames);
        e = std::clamp(e, s, total_frames);
        l = std::clamp(l, s, e);
        return {.m_start = s, .m_end = e, .m_loop_point = l};
    }
};

}  // namespace thl::dsp::granular
