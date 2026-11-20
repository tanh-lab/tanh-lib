#include "tanh/dsp/transport/ContinuityTracker.h"

#include <tanh/core/AbiNamespace.h>
#include <tanh/dsp/transport/TransportInfo.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace thl::inline THL_ABI_NAMESPACE::dsp::transport {

namespace {

constexpr double k_min_tolerance = 1e-6;
constexpr double k_tempo_epsilon = 1e-6;
constexpr double k_default_bpm = 120.0;
// A snapped moving block runs at least at this share of its nominal tempo.
constexpr double k_min_slope_ratio = 0.5;

}  // namespace

void ContinuityTracker::prepare(double sample_rate) {
    if (sample_rate > 0.0) { m_sample_rate = sample_rate; }
    reset();
}

void ContinuityTracker::set_tolerance_samples(double samples) {
    if (samples >= 0.0) { m_tolerance_samples = samples; }
}

void ContinuityTracker::set_tempo_window_samples(double samples) {
    if (samples >= 0.0) { m_tempo_window_samples = samples; }
}

void ContinuityTracker::reset() TANH_NONBLOCKING_FUNCTION {
    m_reset_pending = true;
    m_carry = 0.0;
}

double ContinuityTracker::tolerance_beats(double bpm) const {
    const double per_sample = std::abs(bpm) / (60.0 * m_sample_rate);
    return std::max(m_tolerance_samples * per_sample, k_min_tolerance);
}

void ContinuityTracker::resolve(TransportInfo& io,
                                double raw_end_beat,
                                uint32_t frames) TANH_NONBLOCKING_FUNCTION {
    io.m_flags &= ~TransportInfo::k_discontinuity_mask;
    io.m_jump_delta_beats = 0.0;
    io.m_num_samples = frames;

    const bool playing = io.is_playing();
    if (!std::isfinite(io.m_bpm)) { io.m_bpm = m_prev_bpm > 0.0 ? m_prev_bpm : k_default_bpm; }
    double start = io.m_beat_position;
    double end = raw_end_beat;
    if (!std::isfinite(start) || !std::isfinite(end)) {
        start = m_prev_end;  // hold: m_prev_end is always finite
        end = start;
    }
    double carry = 0.0;

    if (m_reset_pending) {
        io.m_flags |= TransportInfo::k_timeline_reset;
        m_reset_pending = false;
    } else {
        // The start may differ from the previous end by up to the window
        // [min(0, e, -w) - tol, max(0, e, w) + tol] without being a jump:
        // - tol: jitter, N samples worth of beats (set_tolerance_samples()).
        // - e: a source that reports one tempo per block (JUCE, VST3) predicted
        //   the previous end with the previous block's tempo, so a tempo step or
        //   ramp inside that block moved the real position by at most
        //   prev_frames * (bps - prev_bps), in the direction of the change.
        // - w: a change dated up to the window before or after the block start
        //   (a shared session: the change at a peer's output time) moves
        //   the start by up to window * |bps - prev_bps| in either direction. The
        //   window and e cover the same lateness, so they are not added: a change
        //   dated more than the window in the past is still a jump.
        // - carry: how far the source was still behind the previous block's
        //   resolved end when that block was held to the minimum slope.
        const double delta = start - m_prev_end;
        const double jitter = tolerance_beats(io.m_bpm);
        const double tempo_step = (io.m_bpm - m_prev_bpm) / (60.0 * m_sample_rate);
        const double tempo_extra = m_prev_moving ? m_prev_frames * tempo_step : 0.0;
        const double tempo_window =
            m_prev_moving ? m_tempo_window_samples * std::abs(tempo_step) : 0.0;
        const double low = std::min({0.0, tempo_extra, -tempo_window, m_carry}) - jitter;
        const double high = std::max({0.0, tempo_extra, tempo_window, m_carry}) + jitter;
        if (!(delta >= low && delta <= high)) {
            io.m_flags |= TransportInfo::k_jumped;
            io.m_jump_delta_beats = delta;
        } else {
            // Keep the timeline continuous, also across rounding: the start is
            // exactly the previous block's beat_end(), so a division boundary on
            // the edge falls in one block only. A holding source holds at the
            // expected beat; a moving one is re-sloped to land on its raw end,
            // so the difference is caught up within this block and nothing
            // accumulates.
            // A source behind the previous end (a tempo drop inside the previous
            // block) would hold or run backwards on the way: the block runs at
            // least at k_min_slope_ratio of its tempo, and the rest is carried.
            const bool holding = end == start;
            start = m_prev_end;
            if (holding) {
                end = start;
                carry = m_carry;  // still behind by as much when it moves again
            } else if (frames > 0) {
                const double min_end = start + (k_min_slope_ratio * static_cast<double>(frames) *
                                                std::abs(io.m_bpm) / (60.0 * m_sample_rate));
                if (end < min_end) {
                    carry = end - min_end;
                    end = min_end;
                }
            }
        }
        if (playing && !m_prev_playing) { io.m_flags |= TransportInfo::k_started; }
        if (!playing && m_prev_playing) { io.m_flags |= TransportInfo::k_stopped; }
        if (std::abs(io.m_bpm - m_prev_bpm) > k_tempo_epsilon) {
            io.m_flags |= TransportInfo::k_tempo_changed;
        }
    }

    io.m_beat_position = start;
    io.m_beats_per_sample = frames > 0 ? (end - start) / static_cast<double>(frames) : 0.0;

    m_carry = carry;
    m_prev_end = io.beat_end();
    m_prev_frames = static_cast<double>(frames);
    m_prev_moving = frames > 0 && end != start;
    m_prev_bpm = io.m_bpm;
    m_prev_playing = playing;
}

}  // namespace thl::inline THL_ABI_NAMESPACE::dsp::transport
