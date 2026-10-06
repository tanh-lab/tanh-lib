#include "tanh/modulation/MotionRecorder.h"

#include <tanh/dsp/transport/TransportInfo.h>
#include <tanh/modulation/MotionLane.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

namespace thl::modulation {

using thl::dsp::transport::TransportInfo;

namespace {

constexpr size_t k_min_take_points = 4;
constexpr size_t k_max_aligned_bars = 16;
constexpr size_t k_min_capacity = 16;
// Below this the running playback phase is kept as is (bit-exact continuity,
// also across an aligned DAW loop wrap or a hint without a real jump).
constexpr double k_keep_phase_eps = 1e-6;
constexpr double k_tick_eps = 1e-9;
constexpr double k_seam_window_seconds = 0.15;
// A snapped free take counts as still while x and y stay within this distance
// (pad units, |dx| + |dy|) of its first or last point: finger jitter, no move.
constexpr double k_still_distance = 0.002;

// Nearest of 1, 2, 4, 8, 16 bars by ratio (log2), so the stretch is the smallest.
double snap_bars(double bars) {
    if (!(bars > 0.0)) { return 1.0; }
    const double exponent = std::clamp(std::round(std::log2(bars)), 0.0, 4.0);
    return std::exp2(exponent);
}

uint32_t bars_of(LoopLength length) {
    switch (length) {
        case LoopLength::Free: return 0;
        case LoopLength::Bars1: return 1;
        case LoopLength::Bars2: return 2;
        case LoopLength::Bars4: return 4;
        case LoopLength::Bars8: return 8;
        case LoopLength::Bars16: return 16;
    }
    return 0;
}

double circular_distance(double a, double b, double length) {
    const double d = std::abs(a - b);
    return std::min(d, length - d);
}

float clamp01(double v) {
    return static_cast<float>(std::clamp(v, 0.0, 1.0));
}

// Smoothing sigma at amount 1, in lane units (a quarter beat, or 250 ms).
constexpr double k_max_smoothing_sigma = 0.25;
// sigma = max * (base^amount - 1) / (base - 1): fine control at small amounts.
constexpr double k_smoothing_curve_base = 64.0;
// Below this many points of sigma the kernel is a no-op: play the raw lane.
constexpr double k_min_smoothing_points = 0.05;

// Circular convolution of @p in with a sampled Gaussian. The kernel is symmetric
// (zero phase) and wraps around the loop, so the seam is smoothed like any
// other point. Weights sum to 1: the output stays inside [0, 1].
void smooth_circular(const std::vector<float>& in, double sigma, std::vector<float>& out) {
    const size_t n = in.size();
    out.assign(n, 0.0f);
    if (n == 0) { return; }
    // Truncate at 3 sigma; a kernel wider than the loop just wraps more than once.
    const auto radius =
        static_cast<std::ptrdiff_t>(std::min(std::ceil(3.0 * sigma), 2.0 * static_cast<double>(n)));
    std::vector<double> weights(static_cast<size_t>((2 * radius) + 1));
    double sum = 0.0;
    for (std::ptrdiff_t k = -radius; k <= radius; ++k) {
        const auto d = static_cast<double>(k);
        const double w = std::exp(-0.5 * d * d / (sigma * sigma));
        weights[static_cast<size_t>(k + radius)] = w;
        sum += w;
    }
    const auto len = static_cast<std::ptrdiff_t>(n);
    for (std::ptrdiff_t i = 0; i < len; ++i) {
        double acc = 0.0;
        for (std::ptrdiff_t k = -radius; k <= radius; ++k) {
            std::ptrdiff_t j = (i + k) % len;
            if (j < 0) { j += len; }
            acc += weights[static_cast<size_t>(k + radius)] *
                   static_cast<double>(in[static_cast<size_t>(j)]);
        }
        out[static_cast<size_t>(i)] = clamp01(acc / sum);
    }
}

}  // namespace

MotionRecorder::MotionRecorder() : m_audio_reader(&m_lanes.add_reader()) {}

MotionRecorder::~MotionRecorder() = default;

void MotionRecorder::prepare(double sample_rate,
                             size_t max_block_size,
                             const MotionRecorderConfig& config) {
    // Keep what was recorded: publish finished takes before resetting.
    service();
    if (m_take.m_active) {
        abort_take(true);
        m_aborted = true;
    }
    m_armed = false;
    m_force_start = false;
    for (auto& b : m_takes) { b.m_state.store(BufferState::Free, std::memory_order_relaxed); }

    m_config = config;
    m_config.m_max_points = std::max(m_config.m_max_points, k_min_capacity);
    m_config.m_render_interval = std::max<uint32_t>(m_config.m_render_interval, 1);
    if (!(m_config.m_rate > 0.0)) { m_config.m_rate = 200.0; }
    m_config.m_min_tpb = std::max<uint32_t>(m_config.m_min_tpb, 1);
    m_config.m_max_tpb = std::max(m_config.m_max_tpb, m_config.m_min_tpb);
    if (!(m_config.m_glide_ms >= 0.0)) { m_config.m_glide_ms = 25.0; }
    if (sample_rate > 0.0) { m_sample_rate = sample_rate; }

    for (auto& b : m_takes) {
        if (b.m_x.size() != m_config.m_max_points) {
            b.m_x.assign(m_config.m_max_points, 0.0f);
            b.m_y.assign(m_config.m_max_points, 0.0f);
            b.m_gate.assign(m_config.m_max_points, 0);
        }
    }
    m_max_block = max_block_size;
    m_out_x.assign(max_block_size, m_last_x);
    m_out_y.assign(max_block_size, m_last_y);
    m_out_gate.assign(max_block_size, 0);
    m_out_live.assign(max_block_size, 0);
    m_change_points.assign(max_block_size + 1, 0);
    m_num_change_points = 0;
    m_num_samples = 0;
    m_glide_samples = static_cast<uint32_t>(
        std::max(1.0, std::round(m_config.m_glide_ms * m_sample_rate / 1000.0)));

    // Playback re-locks to the next block's beat, without a glide.
    m_source = Source::Lane;
    m_playing_take_id = m_published_id;
    m_need_phase = true;
    m_ramp_restart = true;
    m_ramp_position = 0;
    m_glide_left = 0;
    m_have_clock = false;
    m_jump_glides = 0;
    m_busy = false;
}

size_t MotionRecorder::take_buffer_bytes() const {
    size_t bytes = 0;
    for (const auto& b : m_takes) {
        bytes += (b.m_x.capacity() + b.m_y.capacity()) * sizeof(float) + b.m_gate.capacity();
    }
    return bytes;
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

// Every publication goes through here: smooth on this thread, swap through
// RCU. The audio thread plays m_x / m_y.
void MotionRecorder::publish_played(MotionLane lane) {
    const uint32_t id = lane.m_take_id;
    const double amount = m_smoothing.load(std::memory_order_relaxed);
    const size_t n = lane.num_points();
    double sigma = 0.0;
    if (amount > 0.0 && n > 0 && lane.m_length > 0.0) {
        const double units = k_max_smoothing_sigma *
                             (std::pow(k_smoothing_curve_base, amount) - 1.0) /
                             (k_smoothing_curve_base - 1.0);
        sigma = units * static_cast<double>(n) / lane.m_length;
    }
    const uint32_t version = ++m_played_version;
    m_lanes.replace([&](PlayedLane& l) {
        l.m_smoothed = sigma >= k_min_smoothing_points;
        if (l.m_smoothed) {
            smooth_circular(lane.m_x, sigma, l.m_x);
            smooth_circular(lane.m_y, sigma, l.m_y);
        } else {
            l.m_x = lane.m_x;
            l.m_y = lane.m_y;
        }
        l.m_version = version;
        l.m_lane = std::move(lane);
    });
    m_published_id = id;
    m_lane_version.fetch_add(1, std::memory_order_acq_rel);
}

void MotionRecorder::publish(MotionLane lane, bool record_history) {
    if (record_history) {
        MotionLane previous = this->lane();
        if (!previous.empty() || !lane.empty()) {
            m_undo_lane = std::move(previous);
            m_has_undo = true;
            m_redo_lane = MotionLane{};
            m_has_redo = false;
        }
    }
    publish_played(std::move(lane));
}

bool MotionRecorder::undo() {
    service();
    if (!m_has_undo) { return false; }
    MotionLane current = lane();
    MotionLane restored = std::move(m_undo_lane);
    restored.m_take_id = m_next_take_id.fetch_add(1, std::memory_order_relaxed);
    publish(std::move(restored), false);
    m_undo_lane = MotionLane{};
    m_has_undo = false;
    m_redo_lane = std::move(current);
    m_has_redo = true;
    return true;
}

bool MotionRecorder::redo() {
    service();
    if (!m_has_redo) { return false; }
    MotionLane current = lane();
    MotionLane restored = std::move(m_redo_lane);
    restored.m_take_id = m_next_take_id.fetch_add(1, std::memory_order_relaxed);
    publish(std::move(restored), false);
    m_redo_lane = MotionLane{};
    m_has_redo = false;
    m_undo_lane = std::move(current);
    m_has_undo = true;
    return true;
}

void MotionRecorder::clear_history() {
    m_undo_lane = MotionLane{};
    m_redo_lane = MotionLane{};
    m_has_undo = false;
    m_has_redo = false;
}

void MotionRecorder::set_smoothing(float amount) {
    const float a = std::isfinite(amount) ? std::clamp(amount, 0.0f, 1.0f) : 0.0f;
    if (a == m_smoothing.load(std::memory_order_relaxed)) { return; }
    m_smoothing.store(a, std::memory_order_relaxed);
    publish_played(lane());  // same take id: playback glides to the new curve
}

bool MotionRecorder::service() {
    std::array<TakeBuffer*, 2> finished{};
    size_t count = 0;
    for (auto& b : m_takes) {
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
    if (lane.m_y.size() != n) { return 0; }
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

MotionSnapshot MotionRecorder::snapshot() const TANH_NONBLOCKING_FUNCTION {
    const bool has_lane = m_view_length > 0.0;
    MotionSnapshot s;
    if (m_take.m_active) {
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
    if (m_take.m_active && m_take.m_target > 0) {
        s.m_progress =
            clamp01(static_cast<double>(m_take.m_count) / static_cast<double>(m_take.m_target));
    }
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
        const TakeBuffer& b = m_takes[src == Source::Buffer0 ? 0 : 1];
        v.m_x = b.m_x.data();
        v.m_y = b.m_y.data();
        v.m_gate = b.m_gate.data();
        v.m_num_points = b.m_num_points;
        v.m_timebase = b.m_timebase;
        v.m_length = b.m_length;
        v.m_anchor = b.m_anchor;
    }
    const uint32_t bars = bars_of(m_playback_length);
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

void MotionRecorder::start_glide() {
    m_glide_left = m_glide_samples;
    m_glide_from_x = m_last_x;
    m_glide_from_y = m_last_y;
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
        TakeBuffer& buffer = m_takes[b];
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
                if (m_take.m_active) { break; }
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
                if (m_take.m_active) { end_take(bpm); }
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

void MotionRecorder::start_take(const TransportInfo& t,
                                const MotionInput& in,
                                uint32_t offset,
                                double clock_beat) {
    uint32_t buffer = 2;
    for (uint32_t b = 0; b < 2; ++b) {
        if (m_takes[b].m_state.load(std::memory_order_acquire) == BufferState::Free) {
            buffer = b;
            break;
        }
    }
    if (buffer == 2) {
        m_busy = true;  // both buffers wait for service(); try again next sample
        return;
    }

    const double bpm = t.m_bpm > 0.0 ? t.m_bpm : 120.0;
    const double ideal_tpb = std::clamp(std::round(m_config.m_rate * 60.0 / bpm),
                                        static_cast<double>(m_config.m_min_tpb),
                                        static_cast<double>(m_config.m_max_tpb));
    const uint32_t bars = bars_of(m_arm_length);
    const auto max_points = static_cast<double>(m_config.m_max_points);
    const int sig_num = t.m_sig_num > 0 ? t.m_sig_num : 4;
    const int sig_denom = t.m_sig_denom > 0 ? t.m_sig_denom : 4;
    Take& k = m_take;
    k = Take{};
    k.m_bar_beats = static_cast<double>(sig_num) * 4.0 / static_cast<double>(sig_denom);
    const bool has_in = in.m_x != nullptr && in.m_y != nullptr && offset < in.m_num_samples;
    const float first_x = has_in ? in.m_x[offset] : m_last_x;
    const float first_y = has_in ? in.m_y[offset] : m_last_y;

    if (m_config.m_bar_aligned_takes) {
        // Beats on the transport or the free-running clock. A free take reserves
        // 16 bars, so a whole number of points per bar has to fit 16 times.
        const size_t loop_bars = bars > 0 ? bars : k_max_aligned_bars;
        const double per_bar = std::min(std::round(k.m_bar_beats * ideal_tpb),
                                        std::floor(max_points / static_cast<double>(loop_bars)));
        if (per_bar < (k.m_bar_beats * m_config.m_min_tpb) - k_tick_eps) {
            m_refused = true;
            m_armed = false;
            m_force_start = false;
            return;
        }
        k.m_aligned = bars > 0;
        k.m_snap = bars == 0;
        k.m_bar = bars > 0;
        k.m_beats = true;
        k.m_points_per_bar = static_cast<size_t>(per_bar);
        k.m_capacity = loop_bars * k.m_points_per_bar;
        k.m_target = bars > 0 ? k.m_capacity : k.m_points_per_bar;
        k.m_length = static_cast<double>(loop_bars) * k.m_bar_beats;
        k.m_tick_step = k.m_bar_beats / per_bar;
        // A bar take's point 0 sits on the bar line at or before the touch; a
        // free take's on the first touch (record() rebases it there).
        k.m_bar_line = std::floor((clock_beat / k.m_bar_beats) + k_tick_eps) * k.m_bar_beats;
        k.m_tick_base = bars > 0 ? k.m_bar_line : clock_beat;
        k.m_clock = clock_beat;
    } else if (bars > 0) {
        const int num = t.m_sig_num > 0 ? t.m_sig_num : 4;
        const int den = t.m_sig_denom > 0 ? t.m_sig_denom : 4;
        const double length = bars * static_cast<double>(num) * 4.0 / static_cast<double>(den);
        const double points = std::min(std::round(length * ideal_tpb), max_points);
        if (points < (length * m_config.m_min_tpb) - k_tick_eps) {
            m_refused = true;  // the loop does not fit at the minimum resolution
            m_armed = false;
            m_force_start = false;
            return;
        }
        k.m_bar = true;
        k.m_beats = true;
        k.m_length = length;
        k.m_capacity = static_cast<size_t>(points);
        k.m_tick_step = length / points;
        const double j0 = std::ceil((clock_beat / k.m_tick_step) - k_tick_eps);
        k.m_tick_base = j0 * k.m_tick_step;
        const double wrapped = j0 - (std::floor(j0 / points) * points);
        k.m_start_index = std::min(static_cast<size_t>(wrapped), k.m_capacity - 1);
        k.m_clock = clock_beat;
    } else if (t.is_playing()) {
        k.m_beats = true;
        k.m_capacity = m_config.m_max_points;
        k.m_tick_step = 1.0 / ideal_tpb;
        k.m_tick_base = clock_beat;
        k.m_clock = clock_beat;
    } else {
        k.m_capacity = m_config.m_max_points;
        k.m_tick_step = m_sample_rate / m_config.m_rate;
        k.m_tick_base = 0.0;
        k.m_clock = 0.0;
    }
    if (!k.m_aligned && !k.m_snap) { k.m_target = k.m_capacity; }
    k.m_active = true;
    k.m_buffer = buffer;
    k.m_start_beat = clock_beat;
    k.m_last_x = first_x;
    k.m_last_y = first_y;
    m_takes[buffer].m_state.store(BufferState::Recording, std::memory_order_relaxed);
    if (k.m_aligned) {
        // The time from the bar line to the touch is lifted (gate 0) at the
        // first position. At most one bar of points: a bounded loop.
        const double ticks = std::ceil(((clock_beat - k.m_bar_line) / k.m_tick_step) - k_tick_eps);
        const auto lead = std::min(static_cast<size_t>(std::max(ticks, 0.0)), k.m_target - 1);
        for (size_t i = 0; i < lead; ++i) { write_point(first_x, first_y, 0); }
    }
    m_force_start = false;
    m_busy = false;
    if (m_playing_take_id != 0 && m_view_length > 0.0) { m_replaced_take_id = m_playing_take_id; }
}

void MotionRecorder::write_point(float x, float y, uint8_t gate) {
    Take& k = m_take;
    TakeBuffer& b = m_takes[k.m_buffer];
    const size_t l = k.m_count;
    // Raw points: smoothing is a playback setting (set_smoothing()).
    size_t p = k.m_start_index + l;
    if (p >= k.m_capacity) { p -= k.m_capacity; }
    b.m_x[p] = x;
    b.m_y[p] = y;
    b.m_gate[p] = gate;
    if (l == 0 || x != k.m_last_x || y != k.m_last_y) { k.m_count_at_change = l + 1; }
    k.m_count = l + 1;
    k.m_last_x = x;
    k.m_last_y = y;
}

// A snapped free take loops without a pause: drop the points before the finger
// first moves and after it last moved (each run kept down to one point). The
// shift is a bounded memmove inside the take buffer.
void MotionRecorder::trim_still_ends() {
    Take& k = m_take;
    TakeBuffer& b = m_takes[k.m_buffer];
    const size_t n = k.m_count;
    auto distance = [&](size_t i, size_t j) {
        return std::abs(static_cast<double>(b.m_x[i]) - b.m_x[j]) +
               std::abs(static_cast<double>(b.m_y[i]) - b.m_y[j]);
    };
    size_t first = 0;
    while (first < n && distance(first, 0) <= k_still_distance) { ++first; }
    if (first == n) { return; }  // never moved: a held position, kept as it is
    size_t last = n - 1;
    while (last > 0 && distance(last, n - 1) <= k_still_distance) { --last; }
    const size_t begin = first - 1;
    const size_t end = std::min(n, std::max(last + 2, first + 1));
    if (begin > 0) {
        const auto from = static_cast<std::ptrdiff_t>(begin);
        const auto to = static_cast<std::ptrdiff_t>(end);
        std::copy(b.m_x.begin() + from, b.m_x.begin() + to, b.m_x.begin());
        std::copy(b.m_y.begin() + from, b.m_y.begin() + to, b.m_y.begin());
        std::copy(b.m_gate.begin() + from, b.m_gate.begin() + to, b.m_gate.begin());
    }
    k.m_start_beat += static_cast<double>(begin) * k.m_tick_step;
    k.m_count = end - begin;
}

void MotionRecorder::finalise(TakeBuffer& b, size_t n, double points_per_second) {
    const Take& k = m_take;
    auto phys = [&](size_t logical) {
        const size_t p = k.m_start_index + logical;
        return p >= n ? p - n : p;
    };

    // Seam blend: spread the step at the wrap beyond the expected velocity over
    // the last W points (raised cosine), so the raw loop has no jump at the
    // seam even with smoothing off (a finger held across the loop end).
    if (n >= k_min_take_points) {
        const auto window = static_cast<size_t>(
            std::max(1.0,
                     std::min(std::floor(static_cast<double>(n) / 4.0),
                              std::round(k_seam_window_seconds * points_per_second))));
        auto blend = [&](std::vector<float>& p) {
            const double p0 = p[phys(0)];
            const double pn = p[phys(n - 1)];
            // Expected step across the seam: the mean velocity on both sides
            // (three points each, robust against a single jittery point).
            const double vel =
                n >= 8 ? (((p[phys(3)] - p0) / 3.0) + ((pn - p[phys(n - 4)]) / 3.0)) * 0.5
                       : static_cast<double>(p[phys(1)]) - p0;
            const double e = (p0 - pn) - vel;
            for (size_t j = 0; j < window; ++j) {
                const double w =
                    0.5 * (1.0 - std::cos(std::numbers::pi * static_cast<double>(j + 1) /
                                          static_cast<double>(window)));
                float& v = p[phys(n - window + j)];
                v = clamp01(static_cast<double>(v) + (e * w));
            }
        };
        blend(b.m_x);
        blend(b.m_y);
    }
}

void MotionRecorder::abort_take(bool keep_replaced) {
    if (!m_take.m_active) { return; }
    m_takes[m_take.m_buffer].m_state.store(BufferState::Free, std::memory_order_relaxed);
    m_take.m_active = false;
    m_armed = false;
    m_force_start = false;
    if (m_replaced_take_id != 0) {
        if (keep_replaced) {
            m_need_phase = true;  // the old lane plays on from the clock
        } else {
            m_clear_request.store(m_replaced_take_id, std::memory_order_release);
        }
    }
    if (keep_replaced) { m_replaced_take_id = 0; }
}

void MotionRecorder::finish_take(double bpm, uint32_t trim_samples) {
    Take& k = m_take;
    if (!k.m_active) { return; }
    if (!k.m_bar && !k.m_snap && trim_samples > 0 && k.m_count_at_change >= k_min_take_points) {
        // The release reaches us up to a block after the last move (events carry
        // no timestamps): a still tail no longer than that block is quantisation,
        // not gesture, and would put a pause into the loop.
        const double samples_per_point =
            k.m_beats ? k.m_tick_step * 60.0 * m_sample_rate / bpm : k.m_tick_step;
        const auto tail = static_cast<double>(k.m_count - k.m_count_at_change);
        if (tail * samples_per_point <= static_cast<double>(trim_samples)) {
            k.m_count = k.m_count_at_change;
        }
    }
    if (k.m_snap) { trim_still_ends(); }
    const bool complete = k.m_aligned ? k.m_count == k.m_target
                          : k.m_bar   ? k.m_count == k.m_capacity
                                      : k.m_count >= k_min_take_points;
    if (!complete) {
        abort_take();
        return;
    }
    TakeBuffer& b = m_takes[k.m_buffer];
    const size_t n = k.m_count;
    double points_per_second = 0.0;
    if (k.m_bar && !k.m_aligned) {
        b.m_timebase = MotionTimebase::Beats;
        b.m_length = k.m_length;
        b.m_anchor = 0.0;
        b.m_rate = static_cast<double>(n) / k.m_length;
        points_per_second = b.m_rate * bpm / 60.0;
    } else if (k.m_aligned) {
        // Whole bars from the bar line: index 0 plays on that bar line.
        const double length =
            static_cast<double>(n) / static_cast<double>(k.m_points_per_bar) * k.m_bar_beats;
        b.m_timebase = MotionTimebase::Beats;
        b.m_length = length;
        b.m_anchor = detail::wrap_phase(k.m_bar_line, length);
        b.m_rate = static_cast<double>(n) / length;
        points_per_second = b.m_rate * bpm / 60.0;
    } else if (k.m_snap) {
        // Stretched onto the nearest of 1, 2, 4, 8 or 16 bars; index 0 plays on
        // the bar line nearest the first kept point.
        const double beats = static_cast<double>(n) * k.m_tick_step;
        const double length = snap_bars(beats / k.m_bar_beats) * k.m_bar_beats;
        b.m_timebase = MotionTimebase::Beats;
        b.m_length = length;
        b.m_anchor =
            detail::wrap_phase(std::round(k.m_start_beat / k.m_bar_beats) * k.m_bar_beats, length);
        b.m_rate = static_cast<double>(n) / length;
        points_per_second = b.m_rate * bpm / 60.0;
    } else if (k.m_beats) {
        // Free take while the transport plays: round to whole beats, stretch at read time.
        const double elapsed = static_cast<double>(n) * k.m_tick_step;
        const double length = std::max(1.0, std::round(elapsed));
        b.m_timebase = MotionTimebase::Beats;
        b.m_length = length;
        b.m_anchor = detail::wrap_phase(std::round(k.m_start_beat * 16.0) / 16.0, length);
        b.m_rate = 1.0 / k.m_tick_step;
        points_per_second = b.m_rate * bpm / 60.0;
    } else {
        b.m_timebase = MotionTimebase::Seconds;
        b.m_rate = m_sample_rate / k.m_tick_step;
        b.m_length = static_cast<double>(n) / b.m_rate;
        b.m_anchor = 0.0;
        points_per_second = b.m_rate;
    }
    finalise(b, n, points_per_second);
    b.m_num_points = n;
    b.m_take_id = m_next_take_id.fetch_add(1, std::memory_order_relaxed);
    b.m_state.store(BufferState::Finished, std::memory_order_release);

    k.m_active = false;
    m_armed = false;
    m_force_start = false;
    m_play_enabled = true;
    m_replaced_take_id = 0;
    switch_to(k.m_buffer == 0 ? Source::Buffer0 : Source::Buffer1, b.m_take_id, false);
}

// disarm() / stop() never throw a touched bar-aligned take away: a BarsN take
// ends at once with the rest of its N bars lifted, a free one where it is.
// Padding writes at most m_max_points points into preallocated memory.
void MotionRecorder::end_take(double bpm) {
    const Take& k = m_take;
    if (k.m_snap) {
        if (k.m_saw_touch) {
            finish_take(bpm);
        } else {
            abort_take(true);  // nothing recorded: the replaced lane plays on
        }
        return;
    }
    if (!k.m_aligned) {
        if (k.m_bar) {
            abort_take();
        } else {
            finish_take(bpm);
        }
        return;
    }
    if (!k.m_saw_touch) {
        abort_take(true);  // nothing recorded: the replaced lane plays on
        return;
    }
    while (k.m_count < k.m_target) { write_point(k.m_last_x, k.m_last_y, 0); }
    finish_take(bpm);
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
    const double bpm = t.m_bpm > 0.0 ? t.m_bpm : 120.0;
    const double nominal = bpm / (60.0 * m_sample_rate);
    const bool playing = t.is_playing();
    const bool timeline_reset = t.has(TransportInfo::k_timeline_reset);
    const bool jumped = t.has(TransportInfo::k_jumped) || t.has(TransportInfo::k_started);
    // A timeline reset also restarts the stopped free-run from the transport's beat.
    if (timeline_reset) { m_have_clock = false; }
    const bool hold = m_stopped_mode.load(std::memory_order_relaxed) == StoppedTransport::Hold;
    double b0 = t.m_beat_position;
    double slope = nominal;
    if (playing) {
        if (t.m_beats_per_sample > 0.0) { slope = t.m_beats_per_sample; }
    } else {
        if (m_have_clock) { b0 = m_clock_end; }
        slope = hold ? 0.0 : nominal;
    }
    // A take integrates tempo on its own clock and never stops.
    const double rec_slope = playing && t.m_beats_per_sample > 0.0 ? t.m_beats_per_sample : nominal;
    m_have_clock = true;
    if (t.m_sig_num > 0 && t.m_sig_denom > 0) {
        m_sig_num = t.m_sig_num;
        m_sig_denom = t.m_sig_denom;
    }

    apply_commands(bpm);

    const auto scope = m_lanes.read_scope(*m_audio_reader);
    const PlayedLane& lane = scope.data();
    select_source(lane);

    LaneView view = playback_view(lane);
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
                } else if (timeline_reset) {
                    m_segment_phase = fresh;
                    m_ramp_restart = true;
                } else if (jumped && playing) {
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
        view = playback_view(lane);
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
        if (!m_take.m_active && m_armed && (live || m_force_start)) {
            start_take(t, in, j, b0 + (static_cast<double>(i) * slope));
            if (m_take.m_active && m_replaced_take_id != 0) { view = {}; }
        }
        if (m_take.m_active) {
            Take& k = m_take;
            if (!k.m_bar && k.m_saw_touch && !live) {
                finish_take(bpm, n);  // free take: ends on release
            } else if (k.m_snap && !k.m_saw_touch && !live) {
                // record(Free) without a finger: the take starts at the touch.
                k.m_clock += rec_slope;
            } else {
                if (k.m_snap && !k.m_saw_touch) {
                    k.m_tick_base = k.m_clock;
                    k.m_start_beat = k.m_clock;
                }
                k.m_saw_touch = k.m_saw_touch || live;
                const double next =
                    k.m_tick_base + (static_cast<double>(k.m_count) * k.m_tick_step);
                if (k.m_clock >= next - k_tick_eps) {
                    if (live) {
                        write_point(lx, ly, 1);
                    } else {
                        write_point(k.m_last_x, k.m_last_y, 0);
                    }
                    // A snapped free take ends after 16 bars with the finger
                    // down; its progress runs to the next 1, 2, 4, 8 or 16 bars.
                    if (k.m_count >= k.m_target) {
                        if (k.m_count >= k.m_capacity) {
                            finish_take(bpm);
                        } else {
                            k.m_target = std::min(k.m_target * 2, k.m_capacity);
                        }
                    }
                }
                k.m_clock += k.m_beats ? rec_slope : 1.0;
            }
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
            pg = view.m_gate[gi];
            // The render grid restarts at every loop wrap, so each loop is
            // rendered on the same ticks (loop n == loop 1).
            const double half = 0.5 * view.m_length;
            const bool wrapped =
                dphase > 0.0 ? p < m_last_render_phase - half : p > m_last_render_phase + half;
            m_last_render_phase = p;
            if (wrapped) { m_ramp_restart = true; }
            if (m_ramp_restart || m_ramp_position >= interval) {
                if (m_ramp_restart) {
                    m_ramp_to_x = detail::catmull_rom_wrap(view.m_x, view.m_num_points, p * scale);
                    m_ramp_to_y = detail::catmull_rom_wrap(view.m_y, view.m_num_points, p * scale);
                    m_ramp_restart = false;
                }
                m_ramp_from_x = m_ramp_to_x;
                m_ramp_from_y = m_ramp_to_y;
                double ahead = m_segment_phase + ((k + interval) * dphase);
                if (ahead < 0.0 || ahead >= view.m_length) {
                    ahead = detail::wrap_phase(ahead, view.m_length);
                }
                m_ramp_to_x = detail::catmull_rom_wrap(view.m_x, view.m_num_points, ahead * scale);
                m_ramp_to_y = detail::catmull_rom_wrap(view.m_y, view.m_num_points, ahead * scale);
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
            ox = lx;
            oy = ly;
            og = 1;
            m_glide_left = 0;
        } else if (pb_on) {
            if (m_last_live) {
                start_glide();  // release: glide back to the running playback
                cp = true;
            }
            ox = px;
            oy = py;
            if (m_glide_left > 0) {
                const double w =
                    0.5 * (1.0 - std::cos(std::numbers::pi *
                                          static_cast<double>(m_glide_samples - m_glide_left) /
                                          static_cast<double>(m_glide_samples)));
                ox = static_cast<float>(m_glide_from_x + ((px - m_glide_from_x) * w));
                oy = static_cast<float>(m_glide_from_y + ((py - m_glide_from_y) * w));
                --m_glide_left;
            }
            og = pg;
        } else {
            m_glide_left = 0;
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
    m_clock_end = b0 + (static_cast<double>(n) * slope);
    m_previous_phase_step = dphase;
    m_view_timebase = view.m_timebase;
    m_view_length = view.empty() ? 0.0 : view.m_length;
}

}  // namespace thl::modulation
