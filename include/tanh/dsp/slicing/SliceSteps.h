#pragma once

#include <tanh/dsp/sampler/LoopRegion.h>
#include <tanh/dsp/slicing/SliceMap.h>

#include <algorithm>
#include <cmath>
#include <cstddef>

/**
 * @file SliceSteps.h
 * @brief "Slice space": reading a 0..1 control as equal steps through a
 * SliceMap.
 *
 * With N slices, a position-like value u picks slice floor(u * N) and a
 * boundary-like value (a Start or End marker) picks boundary round(u * N),
 * however long each slice is in time. Anything that sweeps the control (a
 * pad, an LFO, a sequencer) therefore steps through slices evenly. These
 * are helpers for hosts that want that mapping; a SliceMap does not require
 * it.
 */
namespace thl::dsp::slicing {

/// The slice (0..N-1) a step value in [0, 1] picks. An empty map or a NaN
/// picks slice 0.
inline size_t slice_of_step(const SliceMap& map, float u) {
    if (map.m_count == 0 || !std::isfinite(u)) { return 0; }
    auto const n = static_cast<float>(map.m_count);
    auto const k = static_cast<long>(std::floor(std::clamp(u, 0.0f, 1.0f) * n));
    return static_cast<size_t>(std::clamp<long>(k, 0, static_cast<long>(map.m_count) - 1));
}

/// The boundary (0..N) a step value in [0, 1] picks.
inline size_t boundary_of_step(const SliceMap& map, float u) {
    if (map.m_count == 0 || !std::isfinite(u)) { return 0; }
    auto const n = static_cast<float>(map.m_count);
    long const k = std::lround(std::clamp(u, 0.0f, 1.0f) * n);
    return static_cast<size_t>(std::clamp<long>(k, 0, static_cast<long>(map.m_count)));
}

/// A slice-space position v in [0, N] as a fraction of the sample,
/// piecewise linear through the boundaries (v = 2.5 is the middle of slice 3
/// in time).
inline float step_to_norm(const SliceMap& map, double v) {
    if (map.m_count == 0 || !std::isfinite(v)) { return 0.0f; }
    auto const n = static_cast<double>(map.m_count);
    v = std::clamp(v, 0.0, n);
    auto k = static_cast<long>(std::floor(v));
    if (k >= static_cast<long>(map.m_count)) { return 1.0f; }
    k = std::max<long>(k, 0);
    auto const lo = static_cast<double>(map.normalized_bound(static_cast<size_t>(k)));
    auto const hi = static_cast<double>(map.normalized_bound(static_cast<size_t>(k) + 1));
    return static_cast<float>(lo + (v - static_cast<double>(k)) * (hi - lo));
}

/**
 * @brief The region between two boundary-like step values.
 *
 * Start and End pick boundaries; the slices between them play. Both on the
 * same boundary play that one slice (the next one, or the last one at the
 * final boundary). End before Start reverses. There is no separate Loop:
 * the region re-enters at its own start.
 */
inline sampler::LoopRegion region_from_steps(const SliceMap& map,
                                             float start,
                                             float end,
                                             size_t total_frames) {
    auto s = static_cast<long>(boundary_of_step(map, start));
    auto e = static_cast<long>(boundary_of_step(map, end));
    auto const n = static_cast<long>(map.m_count);
    if (s == e) {
        if (s < n) {
            e = s + 1;
        } else {
            s = e - 1;
        }
    }
    bool const reverse = e < s;
    size_t const lo = map.boundary_frame(static_cast<size_t>(std::min(s, e)), total_frames);
    size_t const hi = map.boundary_frame(static_cast<size_t>(std::max(s, e)), total_frames);
    return {.m_start = lo, .m_end = hi, .m_loop_point = lo, .m_reverse = reverse};
}

}  // namespace thl::dsp::slicing
