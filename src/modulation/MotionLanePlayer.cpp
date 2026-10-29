#include "tanh/modulation/detail/MotionLanePlayer.h"

#include <tanh/modulation/MotionLane.h>
#include <tanh/modulation/MotionTransform.h>
#include <tanh/modulation/MotionTypes.h>
#include <tanh/modulation/detail/MotionClock.h>
#include <tanh/modulation/detail/MotionLaneStore.h>
#include <tanh/modulation/detail/MotionTakeRecorder.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "MotionInterpolation.h"

namespace thl::modulation::detail {

namespace {

// Below this the running phase is kept as it is (bit-exact continuity, also across an aligned
// DAW loop wrap).
constexpr double k_keep_phase_eps = 1e-6;
// Grid of a transform that plays alone: the jump detector's "two points"; the transform
// itself is evaluated exactly.
constexpr size_t k_alone_points = 256;

double circular_distance(double a, double b, double length) {
    const double d = std::abs(a - b);
    return std::min(d, length - d);
}

}  // namespace

void MotionLanePlayer::prepare(double sample_rate, uint32_t render_interval) {
    if (sample_rate > 0.0) { m_sample_rate = sample_rate; }
    m_interval = std::max<uint32_t>(render_interval, 1);
    m_jump_glides = 0;
    m_phase = 0.0;
    m_length = 0.0;
    m_view = {};
    reset();
}

void MotionLanePlayer::reset() TANH_NONBLOCKING_FUNCTION {
    m_need_phase = true;
    m_ramp_restart = true;
    m_ramp_position = 0;
    m_glide_next = false;
}

void MotionLanePlayer::restart() TANH_NONBLOCKING_FUNCTION {
    m_need_phase = true;
    m_ramp_restart = true;
    m_glide_next = true;
}

MotionLanePlayer::View MotionLanePlayer::view_of(const MotionLaneSource& source,
                                                 const MotionPlaySettings& settings,
                                                 const MotionTransformInfo& info) const
    TANH_NONBLOCKING_FUNCTION {
    const double bar_beats =
        static_cast<double>(settings.m_sig_num > 0 ? settings.m_sig_num : 4) * 4.0 /
        static_cast<double>(settings.m_sig_denom > 0 ? settings.m_sig_denom : 4);
    const double bars = loop_length_bars(settings.m_length);
    View v;
    const MotionLoopView& lane = source.m_view;
    if (!lane.empty()) {
        v.m_x = lane.m_x;
        v.m_y = lane.m_y;
        v.m_gate = lane.m_gate;
        v.m_num_points = lane.m_num_points;
        v.m_timebase = lane.m_timebase;
        v.m_length = lane.m_length;
        v.m_anchor = lane.m_anchor;
        if (bars > 0.0) {  // stretched onto its bars from a bar line
            v.m_timebase = MotionTimebase::Beats;
            v.m_length = bars * bar_beats;
            v.m_anchor = 0.0;
        }
    }
    if (!info.m_active) { return v; }
    if (info.m_uses_lane && !v.empty()) {
        v.m_transformed = true;
        v.m_open_gate = info.m_opens_gate;
        return v;
    }
    if (info.m_plays_alone) {
        // No lane to follow: the transform loops over the playback length, one bar while Free.
        View alone;
        alone.m_transformed = true;
        alone.m_open_gate = info.m_opens_gate;
        alone.m_num_points = k_alone_points;
        alone.m_timebase = MotionTimebase::Beats;
        alone.m_length = (bars > 0.0 ? bars : 1.0) * bar_beats;
        return alone;
    }
    return info.m_uses_lane ? View{} : v;
}

double MotionLanePlayer::clock_phase(double beat) const TANH_NONBLOCKING_FUNCTION {
    const double rel = beat - m_view.m_anchor;
    return wrap_phase(m_reverse ? -rel : rel, m_view.m_length);
}

bool MotionLanePlayer::begin(const MotionBlockClock& clock,
                             const MotionLaneSource& source,
                             const MotionPlaySettings& settings) TANH_NONBLOCKING_FUNCTION {
    m_clock = clock;
    return start(0, source, settings);
}

bool MotionLanePlayer::switch_to(uint32_t offset,
                                 const MotionLaneSource& source,
                                 const MotionPlaySettings& settings) TANH_NONBLOCKING_FUNCTION {
    // Where the running segment is now; the clock's flags belong to the block start.
    if (!m_view.empty()) {
        m_phase =
            wrap_phase(m_segment_phase + (static_cast<double>(offset - m_segment_start) * m_step),
                       m_view.m_length);
        m_previous_step = m_step;
    }
    m_clock.m_beat = m_clock.beat_at(offset);
    m_clock.m_jumped = false;
    m_clock.m_timeline_reset = false;
    return start(offset, source, settings);
}

bool MotionLanePlayer::start(uint32_t offset,
                             const MotionLaneSource& source,
                             const MotionPlaySettings& settings) TANH_NONBLOCKING_FUNCTION {
    const MotionBlockClock& clock = m_clock;
    const MotionTransformInfo info = settings.m_transform != nullptr
                                         ? settings.m_transform->info(settings.m_transform_settings)
                                         : MotionTransformInfo{};
    bool glide = m_glide_next;
    m_glide_next = false;

    if (source.m_id != m_playing_id) {
        m_need_phase = true;  // another lane
        glide = true;
    } else if (source.m_version != m_version && (source.m_smoothed || m_smoothed)) {
        m_ramp_restart = true;  // the same lane, another curve: keep the phase
        glide = true;
    }
    if (info.m_active != m_transformed || (info.m_active && info.m_curve != m_curve)) {
        m_need_phase = true;  // another path, maybe another loop
        glide = true;
    }
    if (settings.m_length != m_play_length) {
        m_need_phase = true;
        glide = true;
    }
    const bool reversed = settings.m_reverse != m_reverse;
    m_playing_id = source.m_id;
    m_version = source.m_version;
    m_smoothed = source.m_smoothed;
    m_transformed = info.m_active;
    m_curve = info.m_curve;
    m_play_length = settings.m_length;
    m_reverse = settings.m_reverse;
    m_transform = settings.m_transform;
    m_transform_settings = settings.m_transform_settings;
    m_view = view_of(source, settings, info);
    m_segment_start = offset;
    if (m_view.empty()) { return false; }

    if (reversed) {
        // Beats loops mirror the clock's phase (a jump: glide); Seconds loops turn around in place.
        m_ramp_restart = true;
        if (m_view.m_timebase == MotionTimebase::Beats) {
            m_need_phase = true;
            glide = true;
        }
    }
    const bool beats = m_view.m_timebase == MotionTimebase::Beats;
    m_step = beats ? (m_reverse ? -clock.m_slope : clock.m_slope)
                   : (m_reverse ? -1.0 : 1.0) / m_sample_rate;
    // Entering or leaving a held clock: re-anchor the ramp, so a held output stands still.
    if ((m_step == 0.0) != (m_previous_step == 0.0)) { m_ramp_restart = true; }

    if (m_need_phase) {
        m_need_phase = false;
        m_ramp_restart = true;
        m_segment_phase = beats ? clock_phase(clock.m_beat) : 0.0;
    } else if (beats) {
        const double fresh = clock_phase(clock.m_beat);
        const double d = circular_distance(fresh, m_phase, m_view.m_length);
        const double two_points = 2.0 * m_view.m_length / static_cast<double>(m_view.m_num_points);
        m_segment_phase = fresh;
        if (d <= k_keep_phase_eps) {
            m_segment_phase = m_phase;
        } else if (clock.m_timeline_reset) {
            m_ramp_restart = true;
        } else if (clock.m_jumped && clock.m_playing) {
            m_ramp_restart = true;  // never interpolate across a jump
            if (d > two_points) {
                glide = true;
                ++m_jump_glides;
            }
        }
    } else {
        m_segment_phase = m_phase;
    }
    return glide;
}

void MotionLanePlayer::evaluate(double phase, float& x, float& y) const TANH_NONBLOCKING_FUNCTION {
    const double index = phase * static_cast<double>(m_view.m_num_points) / m_view.m_length;
    if (m_view.m_x != nullptr) {
        x = catmull_rom_wrap(m_view.m_x, m_view.m_num_points, index);
        y = catmull_rom_wrap(m_view.m_y, m_view.m_num_points, index);
    } else {
        x = 0.5f;
        y = 0.5f;
    }
    if (m_view.m_transformed && m_transform != nullptr) {
        m_transform->place(m_transform_settings, phase / m_view.m_length, x, y);
    }
}

bool MotionLanePlayer::render(uint32_t i, float& x, float& y, uint8_t& gate)
    TANH_NONBLOCKING_FUNCTION {
    const double length = m_view.m_length;
    const auto k = static_cast<double>(i - m_segment_start);
    double p = m_segment_phase + (k * m_step);
    if (p < 0.0 || p >= length) { p = wrap_phase(p, length); }
    auto gi = static_cast<size_t>(p * static_cast<double>(m_view.m_num_points) / length);
    if (gi >= m_view.m_num_points) { gi = m_view.m_num_points - 1; }
    gate = (m_view.m_open_gate || m_view.m_gate == nullptr) ? 1 : m_view.m_gate[gi];

    // The render grid restarts at every loop wrap, so every loop renders on the same ticks.
    const double half = 0.5 * length;
    const bool wrapped =
        m_step > 0.0 ? p < m_last_render_phase - half : p > m_last_render_phase + half;
    m_last_render_phase = p;
    if (wrapped) { m_ramp_restart = true; }
    bool tick = false;
    if (m_ramp_restart || m_ramp_position >= m_interval) {
        if (m_ramp_restart) {
            evaluate(p, m_ramp_to_x, m_ramp_to_y);
            m_ramp_restart = false;
        }
        m_ramp_from_x = m_ramp_to_x;
        m_ramp_from_y = m_ramp_to_y;
        double ahead = m_segment_phase + ((k + m_interval) * m_step);
        if (ahead < 0.0 || ahead >= length) { ahead = wrap_phase(ahead, length); }
        evaluate(ahead, m_ramp_to_x, m_ramp_to_y);
        m_ramp_position = 0;
        tick = true;
    }
    const float w = static_cast<float>(m_ramp_position) / static_cast<float>(m_interval);
    x = m_ramp_from_x + ((m_ramp_to_x - m_ramp_from_x) * w);
    y = m_ramp_from_y + ((m_ramp_to_y - m_ramp_from_y) * w);
    ++m_ramp_position;
    return tick;
}

void MotionLanePlayer::end(uint32_t num_samples) TANH_NONBLOCKING_FUNCTION {
    if (m_view.empty()) {
        m_phase = 0.0;
        m_length = 0.0;
        return;
    }
    m_phase =
        wrap_phase(m_segment_phase + (static_cast<double>(num_samples - m_segment_start) * m_step),
                   m_view.m_length);
    m_length = m_view.m_length;
    m_timebase = m_view.m_timebase;
    m_previous_step = m_step;
}

}  // namespace thl::modulation::detail
