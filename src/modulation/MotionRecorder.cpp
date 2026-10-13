#include "tanh/modulation/MotionRecorder.h"

#include <tanh/dsp/transport/TransportInfo.h>
#include <tanh/modulation/MotionLane.h>
#include <tanh/modulation/MotionLaneProcessing.h>
#include <tanh/modulation/MotionShape.h>
#include <tanh/modulation/MotionTypes.h>
#include <tanh/modulation/detail/MotionClock.h>
#include <tanh/modulation/detail/MotionTakes.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "MotionInterpolation.h"

namespace thl::modulation {

using thl::dsp::transport::TransportInfo;

namespace {

constexpr size_t k_min_capacity = 16;
// Below this the running playback phase is kept as is (bit-exact continuity,
// also across an aligned DAW loop wrap or a hint without a real jump).
constexpr double k_keep_phase_eps = 1e-6;
// Grid of a shape loop without a take: the jump detector's "two points" and the
// phase the UI shows; shapes themselves are evaluated exactly.
constexpr size_t k_shape_points = 256;

double circular_distance(double a, double b, double length) {
    const double d = std::abs(a - b);
    return std::min(d, length - d);
}

float clamp01(double v) {
    return static_cast<float>(std::clamp(v, 0.0, 1.0));
}

}  // namespace

MotionRecorder::MotionRecorder() {
    m_audio_reader = m_lanes.add_reader();
}

MotionRecorder::~MotionRecorder() = default;

void MotionRecorder::prepare(double sample_rate,
                             size_t samples_per_block,
                             const MotionRecorderConfig& config) {
    // Keep what was recorded: publish finished takes before resetting.
    service();
    if (m_takes.active()) {
        m_takes.abort();
        on_take(TakeOutcome::Cancelled);  // the lane the take replaced plays on
        m_aborted = true;
    }
    m_take_plays = false;
    m_armed = false;
    m_force_start = false;

    m_config = config;
    m_config.m_max_points = std::max(m_config.m_max_points, k_min_capacity);
    m_config.m_render_interval = std::max<uint32_t>(m_config.m_render_interval, 1);
    if (!(m_config.m_rate > 0.0)) { m_config.m_rate = 200.0; }
    m_config.m_min_tpb = std::max<uint32_t>(m_config.m_min_tpb, 1);
    m_config.m_max_tpb = std::max(m_config.m_max_tpb, m_config.m_min_tpb);
    if (!(m_config.m_glide_ms >= 0.0)) { m_config.m_glide_ms = 25.0; }
    if (sample_rate > 0.0) { m_sample_rate = sample_rate; }

    m_takes.prepare(m_config, m_sample_rate);
    m_max_block = samples_per_block;
    m_out_x.assign(samples_per_block, m_last_x);
    m_out_y.assign(samples_per_block, m_last_y);
    m_out_gate.assign(samples_per_block, 0);
    m_out_live.assign(samples_per_block, 0);
    m_change_points.assign(samples_per_block + 1, 0);
    m_num_change_points = 0;
    m_num_samples = 0;
    m_glide.set_length(static_cast<uint32_t>(
        std::max(1.0, std::round(m_config.m_glide_ms * m_sample_rate / 1000.0))));

    // Playback re-locks to the next block's beat, without a glide.
    m_source = Source::Lane;
    m_playing_take_id = m_published_id;
    m_need_phase = true;
    m_ramp_restart = true;
    m_ramp_position = 0;
    m_glide.cancel();
    m_clock.reset();
    m_jump_glides = 0;
    m_busy = false;
}

size_t MotionRecorder::take_buffer_bytes() const {
    return m_takes.buffer_bytes();
}

bool MotionRecorder::arm(LoopLength length) {
    return m_commands.try_push({.m_kind = CommandKind::Arm, .m_arg = static_cast<uint8_t>(length)});
}

bool MotionRecorder::record(LoopLength length) {
    return m_commands.try_push(
        {.m_kind = CommandKind::Record, .m_arg = static_cast<uint8_t>(length)});
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
    return m_commands.try_push(
        {.m_kind = CommandKind::PlaybackLength, .m_arg = static_cast<uint8_t>(length)});
}

// Every publication goes through here: build the played x and y on this
// thread (make_played_curve()), swap through RCU. The audio thread plays m_x /
// m_y; the published lane keeps the raw seam.
void MotionRecorder::publish_played(MotionLane lane) {
    const uint32_t id = lane.m_take_id;
    const float smoothing = m_smoothing.load(std::memory_order_relaxed);
    const LoopEnd loop_end = m_loop_end.load(std::memory_order_relaxed);
    PlayedCurve curve = make_played_curve(lane, smoothing, loop_end);
    PlayedCurve seed;
    if (smoothing > 0.0f) { seed = make_played_curve(lane, 0.0f, loop_end); }
    const uint32_t version = ++m_played_version;
    m_lanes.replace([&](PlayedLane& l) {
        l.m_x = std::move(curve.m_x);
        l.m_y = std::move(curve.m_y);
        l.m_seed_x = std::move(seed.m_x);
        l.m_seed_y = std::move(seed.m_y);
        l.m_smoothed = curve.m_smoothed;
        l.m_version = version;
        l.m_lane = std::move(lane);
    });
    m_published_id = id;
    m_lane_version.fetch_add(1, std::memory_order_acq_rel);
}

void MotionRecorder::publish(MotionLane lane, bool record_history) {
    if (record_history) { m_history.record(this->lane(), lane.empty()); }
    publish_played(std::move(lane));
}

bool MotionRecorder::undo() {
    service();
    auto restored = m_history.undo(lane());
    if (!restored) { return false; }
    restored->m_take_id = m_next_take_id.fetch_add(1, std::memory_order_relaxed);
    publish(std::move(*restored), false);
    return true;
}

bool MotionRecorder::redo() {
    service();
    auto restored = m_history.redo(lane());
    if (!restored) { return false; }
    restored->m_take_id = m_next_take_id.fetch_add(1, std::memory_order_relaxed);
    publish(std::move(*restored), false);
    return true;
}

void MotionRecorder::clear_history() {
    m_history.clear();
}

void MotionRecorder::set_smoothing(float amount) {
    const float a = std::isfinite(amount) ? std::clamp(amount, 0.0f, 1.0f) : 0.0f;
    if (a == m_smoothing.load(std::memory_order_relaxed)) { return; }
    m_smoothing.store(a, std::memory_order_relaxed);
    publish_played(lane());  // same take id: playback glides to the new curve
}

void MotionRecorder::set_loop_end(LoopEnd mode) {
    if (mode == m_loop_end.load(std::memory_order_relaxed)) { return; }
    m_loop_end.store(mode, std::memory_order_relaxed);
    publish_played(lane());  // same take id: playback glides to the new curve
}

bool MotionRecorder::service() {
    std::array<TakeBuffer*, 2> finished{};
    size_t count = 0;
    for (auto& b : m_takes.buffers()) {
        if (b.m_state.load(std::memory_order_acquire) == BufferState::Finished) {
            finished[count++] = &b;
        }
    }
    if (count == 2 && finished[1]->m_take_id < finished[0]->m_take_id) {
        std::swap(finished[0], finished[1]);
    }
    bool published = false;
    for (size_t k = 0; k < count; ++k) {
        TakeBuffer& b = *finished[k];
        // A lane loaded after this take finished is newer: drop the take.
        if (b.m_take_id > m_published_id) {
            MotionLane lane;
            lane.m_take_id = b.m_take_id;
            lane.m_timebase = b.m_timebase;
            lane.m_rate = b.m_rate;
            lane.m_length = b.m_length;
            lane.m_anchor = b.m_anchor;
            const auto n = static_cast<std::ptrdiff_t>(b.m_num_points);
            lane.m_x.assign(b.m_x.begin(), b.m_x.begin() + n);
            lane.m_y.assign(b.m_y.begin(), b.m_y.begin() + n);
            lane.m_gate.assign(b.m_gate.begin(), b.m_gate.begin() + n);
            // Undo the seam blend: the lane keeps the raw recording, and the
            // played copy closes the seam again for the current amount.
            const size_t count = b.m_num_points;
            for (size_t j = 0; j < b.m_seam_window; ++j) {
                const size_t i = (b.m_seam + count - b.m_seam_window + j) % count;
                lane.m_x[i] = b.m_raw_tail_x[j];
                lane.m_y[i] = b.m_raw_tail_y[j];
            }
            lane.m_seam = b.m_seam;
            publish(std::move(lane));
            published = true;
        }
        b.m_state.store(BufferState::Published, std::memory_order_release);
    }
    // A take that replaced the lane was dropped: the lane stays deleted.
    const uint32_t replaced = m_clear_request.exchange(0, std::memory_order_acq_rel);
    if (replaced != 0 && replaced == m_published_id) {
        clear();
        published = true;
    }
    return published;
}

uint32_t MotionRecorder::load_lane(MotionLane lane) {
    const size_t n = lane.num_points();
    if (lane.m_y.size() != n || (n > 0 && lane.m_seam >= n)) { return 0; }
    if (lane.m_gate.size() != n) {
        if (!lane.m_gate.empty()) { return 0; }
        lane.m_gate.assign(n, 1);
    }
    if (n > 0) {
        if (!std::isfinite(lane.m_length) || !(lane.m_length > 0.0) ||
            !std::isfinite(lane.m_anchor) || !std::isfinite(lane.m_rate) || !(lane.m_rate > 0.0)) {
            return 0;
        }
        for (size_t i = 0; i < n; ++i) {
            if (!std::isfinite(lane.m_x[i]) || !std::isfinite(lane.m_y[i])) { return 0; }
            lane.m_x[i] = std::clamp(lane.m_x[i], 0.0f, 1.0f);
            lane.m_y[i] = std::clamp(lane.m_y[i], 0.0f, 1.0f);
            lane.m_gate[i] = lane.m_gate[i] != 0 ? 1 : 0;
        }
        lane.m_anchor = detail::wrap_phase(lane.m_anchor, lane.m_length);
        if (n > m_config.m_max_points) { lane.resample(m_config.m_max_points); }
    }
    lane.m_take_id = m_next_take_id.fetch_add(1, std::memory_order_relaxed);
    const uint32_t id = lane.m_take_id;
    publish(std::move(lane));
    return id;
}

uint32_t MotionRecorder::clear() {
    return load_lane(MotionLane{});
}

MotionLane MotionRecorder::lane() const {
    MotionLane copy;
    m_lanes.read([&](const PlayedLane& l) { copy = l.m_lane; });
    return copy;
}

MotionLane MotionRecorder::played_lane() const {
    MotionLane copy;
    m_lanes.read([&](const PlayedLane& l) {
        copy = l.m_lane;
        copy.m_x = l.m_x;
        copy.m_y = l.m_y;
    });
    return copy;
}

void MotionRecorder::set_shapes(const MotionShapeMix& mix) {
    if (m_shape_mix.store(mix)) { m_shapes_version.fetch_add(1, std::memory_order_release); }
}

MotionShapeMix MotionRecorder::shapes() const {
    return m_shape_mix.load();
}

MotionLane MotionRecorder::played_path(size_t num_points) const {
    return render_motion_path(played_lane(), shapes(), num_points);
}

MotionSnapshot MotionRecorder::snapshot() const TANH_NONBLOCKING_FUNCTION {
    const bool has_lane = m_view_length > 0.0;
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
    s.m_beats = has_lane && m_view_timebase == MotionTimebase::Beats;
    s.m_busy = m_busy;
    s.m_refused = m_refused;
    s.m_aborted = m_aborted;
    s.m_phase = has_lane ? clamp01(m_phase / m_view_length) : 0.0f;
    s.m_progress = m_takes.progress();
    s.m_take_id = has_lane ? m_playing_take_id : 0;
    s.m_length = m_view_length;
    return s;
}

MotionRecorder::LaneView MotionRecorder::view_of(Source src, const PlayedLane& played) const {
    LaneView v;
    if (src == Source::Lane) {
        const MotionLane& lane = played.m_lane;
        v.m_x = played.m_x.data();
        v.m_y = played.m_y.data();
        v.m_gate = lane.m_gate.data();
        v.m_num_points = std::min({lane.m_x.size(),
                                   lane.m_y.size(),
                                   lane.m_gate.size(),
                                   played.m_x.size(),
                                   played.m_y.size()});
        v.m_timebase = lane.m_timebase;
        v.m_length = lane.m_length;
        v.m_anchor = lane.m_anchor;
    } else if (src == Source::Buffer0 || src == Source::Buffer1) {
        // A take buffer plays raw until service() publishes it.
        const TakeBuffer& b = m_takes.buffers()[src == Source::Buffer0 ? 0 : 1];
        v.m_x = b.m_x.data();
        v.m_y = b.m_y.data();
        v.m_gate = b.m_gate.data();
        v.m_num_points = b.m_num_points;
        v.m_timebase = b.m_timebase;
        v.m_length = b.m_length;
        v.m_anchor = b.m_anchor;
    }
    const uint32_t bars = loop_length_bars(m_playback_length);
    if (bars > 0 && !v.empty()) {
        // Stretched onto N bars from a bar line: the grid scale follows the length.
        v.m_timebase = MotionTimebase::Beats;
        v.m_length = bars * static_cast<double>(m_sig_num) * 4.0 / static_cast<double>(m_sig_denom);
        v.m_anchor = 0.0;
    }
    return v;
}

MotionRecorder::LaneView MotionRecorder::playback_view(const PlayedLane& lane) const {
    if (m_replaced_take_id != 0 && m_playing_take_id == m_replaced_take_id) { return {}; }
    return view_of(m_source, lane);
}

MotionRecorder::LaneView MotionRecorder::shaped_view(LaneView lane) const {
    // Shapes pause while a take records: the output and the trail are the recording.
    if (!m_shapes.active() || m_takes.active()) { return lane; }
    if (m_shapes.uses_take() && !lane.empty()) {
        lane.m_shaped = true;
        return lane;
    }
    if (!m_shapes.has_shape()) { return lane; }  // a moved take without a take
    // No take to follow: the shapes loop over the playback length, one bar while it is Free.
    LaneView v;
    v.m_shaped = true;
    v.m_num_points = k_shape_points;
    v.m_timebase = MotionTimebase::Beats;
    v.m_length = std::max<uint32_t>(loop_length_bars(m_playback_length), 1) *
                 static_cast<double>(m_sig_num) * 4.0 / static_cast<double>(m_sig_denom);
    return v;
}

void MotionRecorder::apply_shapes() {
    const MotionShapeMix mix = shapes();
    if (mix.m_a != m_shapes.m_a || mix.m_b != m_shapes.m_b) {
        // Another curve, and maybe another loop: re-seek and glide like a new lane.
        m_need_phase = true;
        m_ramp_restart = true;
        if (!m_last_live) { start_glide(); }
    }
    m_shapes = mix;
    const float morph = std::clamp(mix.m_morph, 0.0f, 1.0f);
    m_shape_gate = (mix.m_a != MotionShape::Take && morph < 1.0f) ||
                   (mix.m_b != MotionShape::Take && morph > 0.0f);
}

// Same lane, other curve (smoothing): keep the phase, glide to the new values.
void MotionRecorder::glide_in_place() {
    m_ramp_restart = true;
    if (!m_last_live) { start_glide(); }
}

void MotionRecorder::switch_to(Source src, uint32_t id, bool silent) {
    m_source = src;
    m_playing_take_id = id;
    if (silent) { return; }
    m_need_phase = true;
    m_ramp_restart = true;
    if (!m_last_live) { start_glide(); }
}

// Take handoff: two take buffers move through Free, Recording, Finished,
// Published and back to Free. The audio thread writes only a Recording buffer,
// finalises it in place (Finished) and plays straight from it. service() copies
// a Finished buffer into a MotionLane, publishes it and marks the buffer
// Published. Take ids come from one counter shared with load_lane(), so the
// newest id always wins.
//
// The switch away from a buffer depends only on the lane this block reads: a
// buffer can turn Published after the read scope opened (service() racing this
// block), and the lane in the scope then still predates it.
void MotionRecorder::select_source(const PlayedLane& played) {
    const uint32_t lane_id = played.m_lane.m_take_id;
    if (m_source == Source::Lane) {
        if (lane_id != m_playing_take_id) {
            switch_to(Source::Lane, lane_id, false);
        } else if (played.m_version != m_playing_version) {
            glide_in_place();  // new smoothing
        }
        m_playing_version = played.m_version;
    } else if (lane_id >= m_playing_take_id) {
        // Same id: the lane is a copy of the buffer, switch without a re-seek
        // (and glide when it plays smoothed). A newer id: a loaded lane
        // replaced the take.
        const bool same = lane_id == m_playing_take_id;
        switch_to(Source::Lane, lane_id, same);
        if (same && played.m_smoothed) { glide_in_place(); }
        m_playing_version = played.m_version;
    }
    for (uint32_t b = 0; b < 2; ++b) {
        const Source own = b == 0 ? Source::Buffer0 : Source::Buffer1;
        TakeBuffer& buffer = m_takes.buffers()[b];
        if (m_source != own &&
            buffer.m_state.load(std::memory_order_acquire) == BufferState::Published) {
            buffer.m_state.store(BufferState::Free, std::memory_order_relaxed);
        }
    }
}

void MotionRecorder::apply_commands(double bpm) {
    Command c;
    while (m_commands.try_pop(c)) {
        switch (c.m_kind) {
            case CommandKind::Arm:
            case CommandKind::Record:
                if (m_takes.active()) { break; }
                m_armed = true;
                m_force_start = c.m_kind == CommandKind::Record;
                m_arm_length = static_cast<LoopLength>(std::min<uint8_t>(c.m_arg, 5));
                m_refused = false;
                m_aborted = false;
                m_busy = false;
                break;
            case CommandKind::Disarm:
            case CommandKind::Stop:
                m_armed = false;
                m_force_start = false;
                m_busy = false;
                if (m_takes.active()) { on_take(m_takes.end(bpm)); }
                if (c.m_kind == CommandKind::Stop) { m_play_enabled = false; }
                break;
            case CommandKind::Play:
                if (!m_play_enabled) {
                    m_play_enabled = true;
                    m_need_phase = true;
                    m_ramp_restart = true;
                    if (!m_last_live) { start_glide(); }
                }
                break;
            case CommandKind::PlaybackLength: {
                const auto length = static_cast<LoopLength>(std::min<uint8_t>(c.m_arg, 5));
                if (length == m_playback_length) { break; }
                m_playback_length = length;
                m_need_phase = true;
                m_ramp_restart = true;
                if (!m_last_live) { start_glide(); }
                break;
            }
            case CommandKind::Reverse: {
                const bool reverse = c.m_arg != 0;
                if (reverse == m_reverse) { break; }
                m_reverse = reverse;
                m_ramp_restart = true;
                // Beats lanes mirror the transport phase (a jump: glide); Seconds
                // lanes turn around where they are.
                m_reverse_pending = true;
                break;
            }
        }
    }
}

// What plays, unsmoothed: overdubs keep it as it was recorded. Only a forward Beats
// loop maps onto the take's grid; anything else starts the take afresh.
detail::TakeSeed MotionRecorder::seed_of(const PlayedLane& played) const {
    if (!m_play_enabled || m_reverse) { return {}; }
    const LaneView v = playback_view(played);
    if (v.empty() || v.m_timebase != MotionTimebase::Beats) { return {}; }
    detail::TakeSeed s{.m_x = v.m_x,
                       .m_y = v.m_y,
                       .m_gate = v.m_gate,
                       .m_num_points = v.m_num_points,
                       .m_length = v.m_length,
                       .m_anchor = v.m_anchor};
    if (m_source == Source::Lane && played.m_seed_x.size() >= v.m_num_points &&
        played.m_seed_y.size() >= v.m_num_points) {
        s.m_x = played.m_seed_x.data();
        s.m_y = played.m_seed_y.data();
    }
    return s;
}

void MotionRecorder::start_take(const TransportInfo& t,
                                const MotionInput& in,
                                uint32_t offset,
                                double clock_beat,
                                const PlayedLane& lane) {
    const bool has_in = in.m_x != nullptr && in.m_y != nullptr && offset < in.m_num_samples;
    const float first_x = has_in ? in.m_x[offset] : m_last_x;
    const float first_y = has_in ? in.m_y[offset] : m_last_y;
    switch (m_takes.start(t, m_arm_length, first_x, first_y, clock_beat, seed_of(lane))) {
        case detail::MotionTakeWriter::Start::Busy:
            m_busy = true;  // both buffers wait for service(); try again next sample
            return;
        case detail::MotionTakeWriter::Start::Refused:
            m_refused = true;  // the loop does not fit at the minimum resolution
            m_armed = false;
            m_force_start = false;
            return;
        case detail::MotionTakeWriter::Start::Started: break;
    }
    m_force_start = false;
    m_busy = false;
    if (m_playing_take_id != 0 && m_view_length > 0.0) { m_replaced_take_id = m_playing_take_id; }
}

void MotionRecorder::on_take(TakeOutcome outcome) {
    switch (outcome) {
        case TakeOutcome::Running: return;
        case TakeOutcome::Finished: {
            m_armed = false;
            m_force_start = false;
            m_play_enabled = true;
            m_replaced_take_id = 0;
            const uint32_t buffer = m_takes.finished_buffer();
            // An overwriting take already plays from this buffer, on the same loop.
            switch_to(buffer == 0 ? Source::Buffer0 : Source::Buffer1,
                      m_takes.buffers()[buffer].m_take_id,
                      m_take_plays);
            m_take_plays = false;
            return;
        }
        case TakeOutcome::Dropped:
            // Nothing kept: service() clears the lane the take replaced.
            m_take_plays = false;
            m_armed = false;
            m_force_start = false;
            if (m_replaced_take_id != 0) {
                m_clear_request.store(m_replaced_take_id, std::memory_order_release);
            }
            return;
        case TakeOutcome::Cancelled:
            // Nothing recorded: the replaced lane plays on from the clock, unless
            // an earlier dropped take deleted it and service() has not cleared it yet.
            m_take_plays = false;
            m_armed = false;
            m_force_start = false;
            if (m_replaced_take_id != 0 &&
                m_clear_request.load(std::memory_order_relaxed) == m_replaced_take_id) {
                return;
            }
            if (m_replaced_take_id != 0) { m_need_phase = true; }
            m_replaced_take_id = 0;
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
    m_num_change_points = 0;
    if (n == 0) { return; }

    // Clock: the transport while it plays, else a free-running (or held) beat.
    // The transport's discontinuity flags are the only jump detector (see
    // "Clock discontinuities" in docs/sphinx/motion_recording.md).
    const detail::MotionBlockClock clock =
        m_clock.begin(t,
                      m_sample_rate,
                      m_stopped_mode.load(std::memory_order_relaxed) == StoppedTransport::Hold);
    const double bpm = clock.m_bpm;
    const double b0 = clock.m_beat;
    const double slope = clock.m_slope;
    if (t.m_sig_num > 0 && t.m_sig_denom > 0) {
        m_sig_num = t.m_sig_num;
        m_sig_denom = t.m_sig_denom;
    }

    apply_commands(bpm);
    apply_shapes();
    m_takes.set_loop_end(m_loop_end.load(std::memory_order_relaxed));

    const auto scope = m_lanes.read_scope(m_audio_reader);
    const PlayedLane& lane = scope.data();
    select_source(lane);

    // An overwriting take plays its own buffer once its whole loop holds motion.
    auto current_view = [&] {
        if (m_takes.playable()) {
            return view_of(m_takes.buffer() == 0 ? Source::Buffer0 : Source::Buffer1, lane);
        }
        return shaped_view(playback_view(lane));
    };
    LaneView view = current_view();
    double dphase = 0.0;
    auto direction = [&](const LaneView& v) {
        if (v.m_timebase == MotionTimebase::Beats) { return m_reverse ? -slope : slope; }
        return (m_reverse ? -1.0 : 1.0) / m_sample_rate;
    };
    auto transport_phase = [&](const LaneView& v, uint32_t offset) {
        const double beat = b0 + (static_cast<double>(offset) * slope);
        const double rel = beat - v.m_anchor;
        return detail::wrap_phase(m_reverse ? -rel : rel, v.m_length);
    };

    if (m_reverse_pending) {
        m_reverse_pending = false;
        if (!view.empty() && view.m_timebase == MotionTimebase::Beats) {
            m_need_phase = true;
            if (!m_last_live) { start_glide(); }
        }
    }

    // Block-start phase. Beats lanes derive it from the clock beat every block.
    // The flags alone decide what a difference to the running phase means:
    // - none: the beat is continuous (the clock absorbed jitter and tempo
    //   changes), so a difference is rounding: take the clock's phase silently;
    // - k_timeline_reset: re-lock without a glide;
    // - k_jumped / k_started (playing): re-lock, restart the render ramp (never
    //   interpolate across the jump) and glide if the phase moved by more than
    //   two lane points; an aligned jump (DAW loop of whole lanes) changes nothing.
    // While stopped the beat is the recorder's own free-run, continuous by
    // construction; transport seeks take effect at the next start.
    m_segment_start = 0;
    if (!view.empty()) {
        dphase = direction(view);
        // Entering or leaving a held clock: re-anchor the render ramp, so a
        // held output stands still from the first held sample.
        if ((dphase == 0.0) != (m_previous_phase_step == 0.0)) { m_ramp_restart = true; }
        if (!m_need_phase) {
            if (view.m_timebase == MotionTimebase::Beats) {
                const double fresh = transport_phase(view, 0);
                const double d = circular_distance(fresh, m_phase, view.m_length);
                const double two_points =
                    2.0 * view.m_length / static_cast<double>(view.m_num_points);
                if (d <= k_keep_phase_eps) {
                    m_segment_phase = m_phase;
                } else if (clock.m_timeline_reset) {
                    m_segment_phase = fresh;
                    m_ramp_restart = true;
                } else if (clock.m_jumped && clock.m_playing) {
                    m_segment_phase = fresh;
                    m_ramp_restart = true;
                    if (d > two_points) {
                        if (!m_last_live) { start_glide(); }
                        ++m_jump_glides;
                    }
                } else {
                    m_segment_phase = fresh;
                }
            } else {
                m_segment_phase = m_phase;
            }
        }
    }

    auto relock = [&](uint32_t offset) {
        view = current_view();
        m_need_phase = false;
        m_segment_start = offset;
        m_ramp_restart = true;
        if (view.empty()) {
            m_segment_phase = 0.0;
            return;
        }
        dphase = direction(view);
        m_segment_phase =
            view.m_timebase == MotionTimebase::Beats ? transport_phase(view, offset) : 0.0;
    };

    const uint32_t in_n =
        (in.m_x != nullptr && in.m_y != nullptr && in.m_active != nullptr) ? in.m_num_samples : 0;
    size_t in_cp = 0;
    const uint32_t interval = m_config.m_render_interval;
    const auto inv_interval = static_cast<float>(1.0 / interval);
    // The played x and y at @p phase: the lane, or the shapes over it.
    auto render = [&](double phase, float& x, float& y) {
        const double index = phase * static_cast<double>(view.m_num_points) / view.m_length;
        if (!view.m_shaped) {
            x = detail::catmull_rom_wrap(view.m_x, view.m_num_points, index);
            y = detail::catmull_rom_wrap(view.m_y, view.m_num_points, index);
            return;
        }
        MotionShapePoint take{.m_x = 0.5f, .m_y = 0.5f};
        if (view.m_x != nullptr) {
            take = {.m_x = detail::catmull_rom_wrap(view.m_x, view.m_num_points, index),
                    .m_y = detail::catmull_rom_wrap(view.m_y, view.m_num_points, index)};
        }
        const MotionShapePoint p = mix_motion_shapes(m_shapes, phase / view.m_length, take);
        x = p.m_x;
        y = p.m_y;
    };

    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t j = i;
        const bool has_in = j < in_n;
        const bool live = has_in && in.m_active[j] != 0;
        const float lx = has_in ? in.m_x[j] : m_last_x;
        const float ly = has_in ? in.m_y[j] : m_last_y;
        bool cp = false;
        while (in_cp < in.m_change_points.size() && in.m_change_points[in_cp] <= j) {
            cp = cp || in.m_change_points[in_cp] == j;
            ++in_cp;
        }

        // Recording.
        if (!m_takes.active() && m_armed && (live || m_force_start)) {
            start_take(t, in, j, b0 + (static_cast<double>(i) * slope), lane);
            if (m_takes.active() && m_replaced_take_id != 0) {
                view = {};
            } else if (m_takes.active() && view.m_shaped) {
                view = playback_view(lane);  // shapes pause during the take
            }
        }
        if (m_takes.active()) { on_take(m_takes.step(live, lx, ly, clock.m_rec_slope, bpm, n)); }
        if (m_takes.playable() && !m_take_plays) {
            // Seeded at its start, or past its first loop line: play what it holds.
            m_take_plays = true;
            m_play_enabled = true;
            m_need_phase = true;
            if (!m_last_live) { start_glide(); }
        }

        if (m_need_phase) {
            relock(i);
            cp = true;
        }

        // Playback.
        const bool pb_on = m_play_enabled && !view.empty();
        float px = m_last_x;
        float py = m_last_y;
        uint8_t pg = 0;
        if (pb_on) {
            const auto np = static_cast<double>(view.m_num_points);
            const double scale = np / view.m_length;
            const auto k = static_cast<double>(i - m_segment_start);
            double p = m_segment_phase + (k * dphase);
            if (p < 0.0 || p >= view.m_length) { p = detail::wrap_phase(p, view.m_length); }
            auto gi = static_cast<size_t>(p * scale);
            if (gi >= view.m_num_points) { gi = view.m_num_points - 1; }
            pg = (m_shape_gate || view.m_gate == nullptr) ? 1 : view.m_gate[gi];
            // The render grid restarts at every loop wrap, so each loop is
            // rendered on the same ticks (loop n == loop 1).
            const double half = 0.5 * view.m_length;
            const bool wrapped =
                dphase > 0.0 ? p < m_last_render_phase - half : p > m_last_render_phase + half;
            m_last_render_phase = p;
            if (wrapped) { m_ramp_restart = true; }
            if (m_ramp_restart || m_ramp_position >= interval) {
                if (m_ramp_restart) {
                    render(p, m_ramp_to_x, m_ramp_to_y);
                    m_ramp_restart = false;
                }
                m_ramp_from_x = m_ramp_to_x;
                m_ramp_from_y = m_ramp_to_y;
                double ahead = m_segment_phase + ((k + interval) * dphase);
                if (ahead < 0.0 || ahead >= view.m_length) {
                    ahead = detail::wrap_phase(ahead, view.m_length);
                }
                render(ahead, m_ramp_to_x, m_ramp_to_y);
                m_ramp_position = 0;
                cp = true;
            }
            const float w = static_cast<float>(m_ramp_position) * inv_interval;
            px = m_ramp_from_x + ((m_ramp_to_x - m_ramp_from_x) * w);
            py = m_ramp_from_y + ((m_ramp_to_y - m_ramp_from_y) * w);
            ++m_ramp_position;
        }

        // Compose: live touch > playback (with glides) > hold.
        float ox = m_last_x;
        float oy = m_last_y;
        uint8_t og = 0;
        if (live) {
            // The touch overrides playback, a shape too, as a gesture overrides automation.
            ox = lx;
            oy = ly;
            og = 1;
            m_glide.cancel();
        } else if (pb_on) {
            if (m_last_live) {
                start_glide();  // release: glide back to the running playback
                cp = true;
            }
            ox = px;
            oy = py;
            m_glide.apply(ox, oy);
            og = pg;
        } else {
            m_glide.cancel();
        }
        if (live != m_last_live || og != m_last_gate) { cp = true; }

        m_out_x[i] = ox;
        m_out_y[i] = oy;
        m_out_gate[i] = og;
        m_out_live[i] = live ? 1 : 0;
        if (cp && (m_num_change_points == 0 || m_change_points[m_num_change_points - 1] != i)) {
            m_change_points[m_num_change_points++] = i;
        }
        m_last_x = ox;
        m_last_y = oy;
        m_last_gate = og;
        m_last_live = live;
    }

    if (!view.empty()) {
        m_phase = detail::wrap_phase(
            m_segment_phase + (static_cast<double>(n - m_segment_start) * dphase),
            view.m_length);
    } else {
        m_phase = 0.0;
    }
    m_clock.end(clock, n);
    m_previous_phase_step = dphase;
    m_view_timebase = view.m_timebase;
    m_view_length = view.empty() ? 0.0 : view.m_length;
}

}  // namespace thl::modulation
