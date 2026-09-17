#pragma once

#include <tanh/dsp/sampler/LoopRegion.h>
#include <tanh/dsp/sampler/SampleView.h>
#include <tanh/dsp/sampler/ZeroCrossing.h>

#include <algorithm>
#include <cstddef>

namespace thl::dsp::sampler {

/**
 * @brief Turns Start / End / Loop markers into a playable LoopRegion.
 *
 * On top of LoopRegion::from_normalized():
 * - **Minimum span:** End keeps its side of Start (End == Start plays
 *   forwards) at least `min_span` frames away; at the sample's edge Start
 *   gives way instead. A zero-length region would be silent.
 * - **Snap** (optional): Start, End and Loop move to the nearest upward zero
 *   crossing within `snap_radius` frames. The direction is always the raw
 *   markers', so a Start snapping past a close End never reverses the
 *   region, and End never snaps back inside the minimum span.
 * - **Joined:** whether both ends of the loop jump (End, and the frame the
 *   head re-enters at) landed on crossings, i.e. the join is in phase and an
 *   equal-gain crossfade is right.
 *
 * The result is cached on its inputs (the markers, the snap flag and the
 * view's first-channel address and length), so an unmoved marker costs
 * nothing. The cache cannot see audio replaced at the same address with the
 * same length: call invalidate() when the sample changes.
 *
 * @par Real-Time Safety
 * resolve() is real-time safe; a cache miss with snap on scans up to
 * 3 * (2 * snap_radius + 1) frames.
 */
class LoopMarkers {
public:
    /// Minimum span and snap radius, in frames.
    void set_spans(size_t min_span, size_t snap_radius) {
        m_min_span = std::max<size_t>(1, min_span);
        m_snap_radius = snap_radius;
        m_cache.m_valid = false;
    }

    /// Forget the cached result (e.g. the sample was replaced in place).
    void invalidate() { m_cache.m_valid = false; }

    /**
     * @brief Resolve normalised markers against `view`.
     * @param joined Set to true when the loop join is in phase (see class).
     */
    LoopRegion resolve(const SampleView& view,
                       float start,
                       float end,
                       float loop,
                       bool snap,
                       bool& joined) {
        size_t const total = view.m_num_frames;
        Frames const raw{.m_start = LoopRegion::to_frame(start, total),
                         .m_end = LoopRegion::to_frame(end, total),
                         .m_loop = LoopRegion::to_frame(loop, total)};
        const float* const data = view.m_channels[0];
        if (m_cache.m_valid && m_cache.m_raw == raw && m_cache.m_data == data &&
            m_cache.m_total == total && m_cache.m_snap == snap) {
            const auto& m = m_cache.m_resolved;
            joined = m_cache.m_joined;
            return LoopRegion::from_frames(m.m_start, m.m_end, m.m_loop, total);
        }

        bool const forward = raw.m_end >= raw.m_start;
        Frames m = raw;
        auto const snapped = [&](size_t target, size_t lo, size_t hi) {
            return nearest_upward_crossing(view, target, lo, hi, m_snap_radius);
        };
        if (snap) { m.m_start = snapped(m.m_start, 0, total); }

        size_t const span = std::min(m_min_span, total);
        if (forward) {
            if (m.m_end < m.m_start + span) {
                m.m_end = m.m_start + span;
                if (m.m_end > total) {
                    m.m_end = total;
                    m.m_start = total - span;
                }
            }
            if (snap) { m.m_end = snapped(m.m_end, m.m_start + span, total); }
        } else {
            if (m.m_end + span > m.m_start) {
                if (m.m_start < span) {
                    m.m_start = span;
                    m.m_end = 0;
                } else {
                    m.m_end = m.m_start - span;
                }
            }
            if (snap) { m.m_end = snapped(m.m_end, 0, m.m_start - span); }
        }
        if (snap) { m.m_loop = snapped(m.m_loop, 0, total); }
        auto const region = LoopRegion::from_frames(m.m_start, m.m_end, m.m_loop, total);
        // The frame the head re-enters at: Start when the region loops whole
        // (Loop outside it or parked on End, see LoopRegion::virtual_loop).
        size_t const reentry =
            region.m_loop_point == region.m_start && m.m_loop != m.m_start ? m.m_start : m.m_loop;
        joined = snap && upward_crossing_at(view, m.m_end) && upward_crossing_at(view, reentry);

        m_cache = {.m_raw = raw,
                   .m_resolved = m,
                   .m_data = data,
                   .m_total = total,
                   .m_snap = snap,
                   .m_joined = joined,
                   .m_valid = true};
        return region;
    }

private:
    struct Frames {
        size_t m_start{0};
        size_t m_end{0};
        size_t m_loop{0};
        bool operator==(const Frames&) const = default;
    };
    struct Cache {
        Frames m_raw{};
        Frames m_resolved{};
        const float* m_data{nullptr};  // the sample's first channel: a new sample misses
        size_t m_total{0};
        bool m_snap{false};
        bool m_joined{false};
        bool m_valid{false};
    };

    size_t m_min_span{1};
    size_t m_snap_radius{0};
    Cache m_cache{};
};

}  // namespace thl::dsp::sampler
