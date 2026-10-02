#include <tanh/dsp/transport/TransportInfo.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace thl::dsp::transport {

namespace {

// Below this the raw beat is taken as is: exact clocks (internal, free-run)
// differ from the predicted end only by rounding, and snapping would move them
// off their closed form.
constexpr double k_exact_epsilon = 1e-9;
constexpr double k_min_tolerance = 1e-6;
constexpr double k_tempo_epsilon = 1e-6;

}  // namespace

void ContinuityTracker::prepare(double sample_rate) noexcept {
    if (sample_rate > 0.0) { m_sample_rate = sample_rate; }
    reset();
}

void ContinuityTracker::set_tolerance_samples(double samples) noexcept {
    if (samples >= 0.0) { m_tolerance_samples = samples; }
}

void ContinuityTracker::reset() noexcept TANH_NONBLOCKING_FUNCTION {
    m_reset_pending = true;
}

double ContinuityTracker::tolerance_beats(double bpm) const noexcept TANH_NONBLOCKING_FUNCTION {
    const double per_sample = std::abs(bpm) / (60.0 * m_sample_rate);
    return std::max(m_tolerance_samples * per_sample, k_min_tolerance);
}

void ContinuityTracker::resolve(TransportInfo& io,
                                double raw_end_beat,
                                uint32_t frames) noexcept TANH_NONBLOCKING_FUNCTION {
    io.m_flags &= ~TransportInfo::k_discontinuity_mask;
    io.m_jump_delta_beats = 0.0;
    io.m_num_samples = frames;

    const bool playing = io.is_playing();
    double start = io.m_beat_position;
    double end = raw_end_beat;

    if (m_reset_pending) {
        io.m_flags |= TransportInfo::k_timeline_reset;
        m_reset_pending = false;
    } else {
        const double delta = start - m_prev_end;
        if (std::abs(delta) > tolerance_beats(io.m_bpm)) {
            io.m_flags |= TransportInfo::k_jumped;
            io.m_jump_delta_beats = delta;
        } else if (std::abs(delta) > k_exact_epsilon) {
            // Jitter: keep the timeline continuous. A holding source (end ==
            // start) holds at the expected beat; a moving one is re-sloped to
            // land on its raw end so the error never accumulates.
            const bool holding = end == start;
            start = m_prev_end;
            if (holding) { end = start; }
        }
        if (playing && !m_prev_playing) { io.m_flags |= TransportInfo::k_started; }
        if (!playing && m_prev_playing) { io.m_flags |= TransportInfo::k_stopped; }
        if (std::abs(io.m_bpm - m_prev_bpm) > k_tempo_epsilon) {
            io.m_flags |= TransportInfo::k_tempo_changed;
        }
    }

    io.m_beat_position = start;
    io.m_beats_per_sample = frames > 0 ? (end - start) / static_cast<double>(frames) : 0.0;

    m_prev_end = frames > 0 ? end : start;
    m_prev_bpm = io.m_bpm;
    m_prev_playing = playing;
}

}  // namespace thl::dsp::transport
