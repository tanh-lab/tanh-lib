#include "tanh/modulation/MotionRecorder.h"

#include <tanh/dsp/transport/TransportInfo.h>
#include <tanh/modulation/MotionLane.h>
#include <tanh/modulation/MotionTransform.h>
#include <tanh/modulation/MotionTypes.h>
#include <tanh/modulation/detail/MotionClock.h>
#include <tanh/modulation/detail/MotionLanePlayer.h>
#include <tanh/modulation/detail/MotionLaneStore.h>
#include <tanh/modulation/detail/MotionTakeRecorder.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>

namespace thl::modulation {

using thl::dsp::transport::TransportInfo;

namespace {

constexpr size_t k_min_points = 16;

float clamp01(double v) {
    return static_cast<float>(std::clamp(v, 0.0, 1.0));
}

}  // namespace

void MotionRecorder::prepare(double sample_rate,
                             size_t samples_per_block,
                             const MotionRecorderConfig& config) {
    // Keep what was recorded: publish handed-over takes before resetting.
    m_store.service();
    if (m_takes.active()) {
        m_takes.abort();  // the lane that played plays on
        m_aborted = true;
    }
    m_replaced_id = 0;
    m_armed = false;
    m_force_start = false;
    m_finish_pending = false;

    m_config = config;
    m_config.m_max_points = std::max(m_config.m_max_points, k_min_points);
    m_config.m_render_interval = std::max<uint32_t>(m_config.m_render_interval, 1);
    if (!(m_config.m_rate > 0.0)) { m_config.m_rate = 200.0; }
    m_config.m_min_tpb = std::max<uint32_t>(m_config.m_min_tpb, 1);
    m_config.m_max_tpb = std::max(m_config.m_max_tpb, m_config.m_min_tpb);
    if (!(m_config.m_glide_ms >= 0.0)) { m_config.m_glide_ms = 25.0; }
    if (sample_rate > 0.0) { m_sample_rate = sample_rate; }

    m_takes.prepare(m_config);
    m_store.prepare(m_config.m_max_points);
    m_player.prepare(m_sample_rate, m_config.m_render_interval);
    m_output.prepare(m_sample_rate, m_config.m_glide_ms, samples_per_block);
    m_clock.reset();
    m_max_block = samples_per_block;
    m_num_samples = 0;
}

size_t MotionRecorder::take_buffer_bytes() const {
    // The take, plus two hand-over slots of the same points and their raw seam tails.
    const size_t points = m_config.m_max_points;
    return m_takes.buffer_bytes() +
           (2 * ((points * ((2 * sizeof(float)) + 1)) + ((points / 4) * 2 * sizeof(float))));
}

bool MotionRecorder::arm(LoopLength length) {
    return m_commands.try_push({.m_kind = CommandKind::Arm, .m_length = length});
}

bool MotionRecorder::record(LoopLength length) {
    return m_commands.try_push({.m_kind = CommandKind::Record, .m_length = length});
}

bool MotionRecorder::disarm() {
    return m_commands.try_push({.m_kind = CommandKind::Disarm, .m_arg = 0});
}

bool MotionRecorder::play() {
    return m_commands.try_push({.m_kind = CommandKind::Play, .m_arg = 0});
}

bool MotionRecorder::stop() {
    return m_commands.try_push({.m_kind = CommandKind::Stop, .m_arg = 0});
}

bool MotionRecorder::set_reverse(bool reverse) {
    return m_commands.try_push(
        {.m_kind = CommandKind::Reverse, .m_arg = static_cast<uint8_t>(reverse ? 1 : 0)});
}

bool MotionRecorder::set_playback_length(LoopLength length) {
    return m_commands.try_push({.m_kind = CommandKind::PlaybackLength, .m_length = length});
}

bool MotionRecorder::service() {
    bool published = m_store.service();
    // A take that kept nothing deleted the lane it replaced, unless a newer one came since.
    const uint32_t replaced = m_clear_request.exchange(0, std::memory_order_acq_rel);
    if (replaced != 0) {
        uint32_t id = 0;
        m_store.read_lane([&](const MotionLane& lane) { id = lane.m_take_id; });
        if (id == replaced) {
            m_store.clear();
            published = true;
        }
    }
    return published;
}

uint32_t MotionRecorder::clear() {
    // A looper records until disarm: what its take holds must not come back after the clear,
    // nor what a take starting before the audio thread sees the clear hands over.
    m_store.drop_takes_from(m_take_serial.load(std::memory_order_acquire));
    m_clear_takes.store(true, std::memory_order_release);
    return m_store.clear();
}

void MotionRecorder::set_transform_settings(uint64_t settings) {
    if (m_transform_settings.exchange(settings, std::memory_order_acq_rel) != settings) {
        m_transform_version.fetch_add(1, std::memory_order_acq_rel);
    }
}

MotionLane MotionRecorder::played_path(size_t num_points) const {
    return render_motion_path(played_lane(), m_transform, transform_settings(), num_points);
}

MotionSnapshot MotionRecorder::snapshot() const TANH_NONBLOCKING_FUNCTION {
    const double length = m_player.length();
    const bool has_lane = length > 0.0;
    MotionSnapshot s;
    if (m_takes.active()) {
        s.m_state = MotionState::Recording;
    } else if (m_armed) {
        s.m_state = MotionState::Armed;
    } else if (m_play_enabled && has_lane) {
        s.m_state = MotionState::Playing;
    }
    s.m_playing = m_play_enabled;
    s.m_reverse = m_reverse;
    s.m_beats = has_lane && m_player.timebase() == MotionTimebase::Beats;
    s.m_busy = m_finish_pending;
    s.m_refused = m_refused;
    s.m_aborted = m_aborted;
    s.m_phase = has_lane ? clamp01(m_player.phase() / length) : 0.0f;
    s.m_progress = m_takes.progress();
    s.m_take_id = has_lane ? m_player.take_id() : 0;
    s.m_length = length;
    return s;
}

void MotionRecorder::apply_commands() TANH_NONBLOCKING_FUNCTION {
    Command c;
    while (m_commands.try_pop(c)) {
        switch (c.m_kind) {
            case CommandKind::Arm:
            case CommandKind::Record:
                if (m_takes.active()) { break; }
                m_armed = true;
                m_force_start = c.m_kind == CommandKind::Record;
                m_arm_length = c.m_length;
                m_refused = false;
                m_aborted = false;
                break;
            case CommandKind::Disarm:
            case CommandKind::Stop:
                m_armed = false;
                m_force_start = false;
                if (m_takes.active()) { on_take(m_takes.end()); }
                if (c.m_kind == CommandKind::Stop) { m_play_enabled = false; }
                break;
            case CommandKind::Play: m_play_enabled = true; break;
            case CommandKind::PlaybackLength: m_playback_length = c.m_length; break;
            case CommandKind::Reverse: m_reverse = c.m_arg != 0; break;
        }
    }
}

void MotionRecorder::start_take(const TransportInfo& t,
                                double beat,
                                float x,
                                float y,
                                const detail::MotionLaneSource& playing) TANH_NONBLOCKING_FUNCTION {
    // A new take starts empty: the lane that played stops now and is replaced by the take, or
    // deleted if the take keeps nothing.
    const detail::MotionTakeStart start{.m_length = m_arm_length,
                                        .m_bpm = t.m_bpm,
                                        .m_sig_num = m_sig_num,
                                        .m_sig_denom = m_sig_denom,
                                        .m_beat = beat,
                                        .m_x = x,
                                        .m_y = y};
    if (m_takes.start(start) == detail::MotionTakeRecorder::Start::Refused) {
        m_refused = true;  // the loop does not fit at the minimum resolution
        m_armed = false;
        m_force_start = false;
        return;
    }
    m_take_serial.fetch_add(1, std::memory_order_acq_rel);
    m_force_start = false;
    if (!playing.empty()) {
        m_replaced_id = playing.m_id;
        m_handed_over = true;  // playback stops from this sample
    }
}

void MotionRecorder::submit_finished() TANH_NONBLOCKING_FUNCTION {
    m_finish_pending =
        !m_store.submit(m_takes.take(), m_take_serial.load(std::memory_order_relaxed));
    m_handed_over = m_handed_over || !m_finish_pending;
}

void MotionRecorder::on_take(TakeEvent event) TANH_NONBLOCKING_FUNCTION {
    switch (event) {
        case TakeEvent::None: return;
        case TakeEvent::Revision:
            // A busy hand-over skips it: the next revision or the end carries the loop.
            m_handed_over =
                m_store.submit(m_takes.take(), m_take_serial.load(std::memory_order_relaxed)) ||
                m_handed_over;
            m_play_enabled = true;
            return;
        case TakeEvent::Finished:
            m_armed = false;
            m_force_start = false;
            m_play_enabled = true;
            submit_finished();
            return;
        case TakeEvent::Dropped:
        case TakeEvent::Cancelled:
            // Nothing kept: the lane it replaced stays deleted (service() publishes the empty
            // lane; undo brings it back).
            m_armed = false;
            m_force_start = false;
            if (m_replaced_id != 0) {
                m_clear_request.store(m_replaced_id, std::memory_order_release);
            }
            return;
    }
}

void MotionRecorder::process(const TransportInfo& t,
                             const MotionInput& in,
                             uint32_t num_samples) TANH_NONBLOCKING_FUNCTION {
    // The buffers hold the prepared maximum; XYController splits larger blocks.
    assert(num_samples <= m_max_block && "MotionRecorder: block exceeds the prepared size");
    const auto n = static_cast<uint32_t>(std::min<size_t>(num_samples, m_max_block));
    m_num_samples = n;
    m_output.begin(false);
    if (n == 0) { return; }

    // The transport while it plays, else a free-running (or held) beat.
    const detail::MotionBlockClock clock =
        m_clock.begin(t,
                      m_sample_rate,
                      m_stopped_mode.load(std::memory_order_relaxed) == StoppedTransport::Hold);
    if (t.m_sig_num > 0 && t.m_sig_denom > 0) {
        m_sig_num = t.m_sig_num;
        m_sig_denom = t.m_sig_denom;
    }
    if (m_clear_takes.exchange(false, std::memory_order_acq_rel)) {
        // clear(): the take keeps nothing, arming ends, later takes play again.
        m_takes.abort();
        m_armed = false;
        m_force_start = false;
        m_finish_pending = false;
        m_store.stop_dropping_after(m_take_serial.load(std::memory_order_relaxed));
    }
    apply_commands();

    // What plays: the newest of the published lane and the handed-over takes.
    const auto scope = m_store.read_scope();
    const uint64_t transform_settings = m_transform_settings.load(std::memory_order_acquire);
    auto settings = [&] {
        return detail::MotionPlaySettings{.m_length = m_playback_length,
                                          .m_reverse = m_reverse,
                                          .m_sig_num = m_sig_num,
                                          .m_sig_denom = m_sig_denom,
                                          .m_transform = m_play_enabled ? m_transform : nullptr,
                                          .m_transform_settings = transform_settings};
    };
    detail::MotionLaneSource playing = m_store.select(scope.data());
    if (m_finish_pending) {  // select() freed the slots service() published
        submit_finished();
        if (m_handed_over) { playing = m_store.select(scope.data()); }
    }
    m_handed_over = false;
    // What plays: nothing while stopped, nor the lane a take replaced.
    auto audible = [&](const detail::MotionLaneSource& source) {
        return m_play_enabled && source.m_id != m_replaced_id ? source : detail::MotionLaneSource{};
    };
    const bool glide = m_player.begin(clock, audible(playing), settings());
    m_output.begin(glide);

    TransportInfo info = t;
    info.m_bpm = clock.m_bpm;
    const uint32_t in_n =
        (in.m_x != nullptr && in.m_y != nullptr && in.m_active != nullptr) ? in.m_num_samples : 0;
    size_t in_cp = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const bool has_in = i < in_n;
        const bool live = has_in && in.m_active[i] != 0;
        const float lx = has_in ? in.m_x[i] : m_output.last_x();
        const float ly = has_in ? in.m_y[i] : m_output.last_y();
        bool cp = false;
        while (in_cp < in.m_change_points.size() && in.m_change_points[in_cp] <= i) {
            cp = cp || in.m_change_points[in_cp] == i;
            ++in_cp;
        }

        // Recording: a take hands its loop over; it plays from the next block. It records what
        // plays at the finger through the transform, so the take plays where the finger drew.
        float rx = lx;
        float ry = ly;
        if (has_in && m_transform != nullptr && !m_transform->unplace(transform_settings, rx, ry)) {
            rx = lx;
            ry = ly;
        }
        if (!m_takes.active() && m_armed && !m_finish_pending && (live || m_force_start)) {
            // Lifted before the touch, at the touch (record() without one: where the output is).
            start_take(info,
                       clock.m_beat + (static_cast<double>(i) * clock.m_rec_slope),
                       rx,
                       ry,
                       playing);
        }
        if (m_takes.active()) { on_take(m_takes.step(live, rx, ry, clock.m_rec_slope)); }
        if (m_handed_over) {  // plays from this sample
            m_handed_over = false;
            playing = m_store.select(scope.data());
            if (m_player.switch_to(i, audible(playing), settings())) { m_output.glide(); }
        }

        // Playback, then the output.
        float px = 0.0f;
        float py = 0.0f;
        uint8_t pg = 0;
        const bool pb = m_player.playing();
        if (pb && m_player.render(i, px, py, pg)) { cp = true; }
        m_output.sample(i, live, lx, ly, pb, px, py, pg, cp);
    }
    m_player.end(n);
    m_clock.end(clock, n);
}

}  // namespace thl::modulation
