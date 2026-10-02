#include <tanh/modulation/MotionRecorder.h>

#include <algorithm>
#include <array>
#include <atomic>
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
constexpr size_t k_min_capacity = 16;
// Below this the running playback phase is kept as is (bit-exact continuity
// across an aligned DAW loop wrap); above it, up to two lane points, the phase
// snaps to the transport silently; beyond two points it glides.
constexpr double k_keep_phase_eps = 1e-6;
constexpr double k_tick_eps = 1e-9;
constexpr double k_seam_window_seconds = 0.15;

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

double circular_distance(double a, double b, double length) noexcept TANH_NONBLOCKING_FUNCTION {
    const double d = std::abs(a - b);
    return std::min(d, length - d);
}

float clamp01(double v) noexcept TANH_NONBLOCKING_FUNCTION {
    return static_cast<float>(std::clamp(v, 0.0, 1.0));
}

}  // namespace

// ── MotionRecorderOutput ──────────────────────────────────────────────────────

MotionRecorderOutput::MotionRecorderOutput(MotionRecorder& recorder, XYPadAxis axis)
    : ModulationSource(k_global_scope, /*fully_active=*/false)
    , m_recorder(recorder)
    , m_axis(axis) {}

void MotionRecorderOutput::prepare(double /*sample_rate*/,
                                   size_t samples_per_block,
                                   uint32_t voice_count) {
    resize_buffers(samples_per_block, voice_count);
}

void MotionRecorderOutput::pre_process_block() {
    const size_t bs = block_size();
    if (bs == 0) { return; }
    const MotionRecorder& r = m_recorder;
    const size_t n = std::min<size_t>(r.m_num_samples, bs);
    float* out = m_output_buffer.data();
    uint8_t* mask = get_output_active().data();
    const float* src = m_axis == XYPadAxis::X ? r.m_out_x.data() : r.m_out_y.data();
    for (size_t i = 0; i < n; ++i) {
        mask[i] = r.m_out_gate[i];
        out[i] = m_axis == XYPadAxis::Active ? static_cast<float>(r.m_out_gate[i]) : src[i];
    }
    if (n == 0) {
        std::fill_n(out, bs, m_last_output);
        std::fill_n(mask, bs, uint8_t{0});
    } else if (n < bs) {
        std::fill(out + n, out + bs, out[n - 1]);
        std::fill(mask + n, mask + bs, mask[n - 1]);
    }
    for (const uint32_t cp : r.change_points()) {
        if (cp < n) { record_change_point(cp); }
    }
    m_last_output = out[bs - 1];
}

// ── MotionRecorder: setup ─────────────────────────────────────────────────────

MotionRecorder::MotionRecorder()
    : m_audio_reader(&m_lanes.add_reader())
    , m_src_x(*this, XYPadAxis::X)
    , m_src_y(*this, XYPadAxis::Y)
    , m_src_gate(*this, XYPadAxis::Active) {}

MotionRecorder::~MotionRecorder() = default;

void MotionRecorder::prepare(double sample_rate,
                             size_t max_block_size,
                             const MotionRecorderConfig& config) {
    // Keep what was recorded: publish finished takes before resetting.
    service();
    if (m_take.m_active) {
        m_takes[m_take.m_buffer].m_state.store(BufferState::Free, std::memory_order_relaxed);
        m_take.m_active = false;
        m_aborted = true;
    }
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
    m_cps.assign(max_block_size + 1, 0);
    m_num_cps = 0;
    m_num_samples = 0;
    m_glide_samples = static_cast<uint32_t>(
        std::max(1.0, std::round(m_config.m_glide_ms * m_sample_rate / 1000.0)));

    // Playback re-locks to the next block's beat, without a glide.
    m_src = Source::Lane;
    m_play_id = m_published_id;
    m_need_phase = true;
    m_ramp_restart = true;
    m_ramp_pos = 0;
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

ModulationSource& MotionRecorder::source(XYPadAxis axis) {
    switch (axis) {
        case XYPadAxis::X: return m_src_x;
        case XYPadAxis::Y: return m_src_y;
        case XYPadAxis::Active: break;
    }
    return m_src_gate;
}

// ── UI thread ─────────────────────────────────────────────────────────────────

bool MotionRecorder::arm(LoopLength length) {
    return m_commands.try_push({CommandKind::Arm, static_cast<uint8_t>(length)});
}

bool MotionRecorder::record(LoopLength length) {
    return m_commands.try_push({CommandKind::Record, static_cast<uint8_t>(length)});
}

bool MotionRecorder::disarm() {
    return m_commands.try_push({CommandKind::Disarm, 0});
}

bool MotionRecorder::play() {
    return m_commands.try_push({CommandKind::Play, 0});
}

bool MotionRecorder::stop() {
    return m_commands.try_push({CommandKind::Stop, 0});
}

bool MotionRecorder::set_reverse(bool reverse) {
    return m_commands.try_push({CommandKind::Reverse, static_cast<uint8_t>(reverse ? 1 : 0)});
}

// ── Message thread ────────────────────────────────────────────────────────────

void MotionRecorder::publish(MotionLane lane) {
    const uint32_t id = lane.m_take_id;
    m_lanes.replace([&](MotionLane& l) { l = std::move(lane); });
    m_published_id = id;
    m_lane_version.fetch_add(1, std::memory_order_acq_rel);
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
    m_lanes.read([&](const MotionLane& l) { copy = l; });
    return copy;
}

MotionSnapshot MotionRecorder::ui_snapshot() const {
    const uint64_t bits = m_snap_bits.load(std::memory_order_acquire);
    MotionSnapshot s;
    s.m_state = static_cast<MotionState>(bits & 0xFu);
    s.m_playing = ((bits >> 4) & 1u) != 0;
    s.m_reverse = ((bits >> 5) & 1u) != 0;
    s.m_beats = ((bits >> 6) & 1u) != 0;
    s.m_busy = ((bits >> 7) & 1u) != 0;
    s.m_refused = ((bits >> 8) & 1u) != 0;
    s.m_aborted = ((bits >> 9) & 1u) != 0;
    s.m_phase = static_cast<float>((bits >> 16) & 0xFFFFu) / 65535.0f;
    s.m_progress = static_cast<float>((bits >> 32) & 0xFFFFu) / 65535.0f;
    s.m_take_id = m_snap_take.load(std::memory_order_relaxed);
    s.m_length = m_snap_length.load(std::memory_order_relaxed);
    return s;
}

// ── Audio thread ──────────────────────────────────────────────────────────────

MotionState MotionRecorder::state() const noexcept TANH_NONBLOCKING_FUNCTION {
    if (m_take.m_active) { return MotionState::Recording; }
    if (m_armed) { return MotionState::Armed; }
    if (m_play_enabled && m_snap_has_lane) { return MotionState::Playing; }
    return MotionState::Idle;
}

MotionRecorder::LaneView MotionRecorder::view_of(Source src, const MotionLane& lane) const noexcept
    TANH_NONBLOCKING_FUNCTION {
    LaneView v;
    if (src == Source::Lane) {
        v.m_x = lane.m_x.data();
        v.m_y = lane.m_y.data();
        v.m_gate = lane.m_gate.data();
        v.m_num_points = std::min({lane.m_x.size(), lane.m_y.size(), lane.m_gate.size()});
        v.m_timebase = lane.m_timebase;
        v.m_length = lane.m_length;
        v.m_anchor = lane.m_anchor;
    } else if (src == Source::Buffer0 || src == Source::Buffer1) {
        const TakeBuffer& b = m_takes[src == Source::Buffer0 ? 0 : 1];
        v.m_x = b.m_x.data();
        v.m_y = b.m_y.data();
        v.m_gate = b.m_gate.data();
        v.m_num_points = b.m_num_points;
        v.m_timebase = b.m_timebase;
        v.m_length = b.m_length;
        v.m_anchor = b.m_anchor;
    }
    return v;
}

void MotionRecorder::start_glide() noexcept TANH_NONBLOCKING_FUNCTION {
    m_glide_left = m_glide_samples;
    m_glide_from_x = m_last_x;
    m_glide_from_y = m_last_y;
}

void MotionRecorder::switch_to(Source src,
                               uint32_t id,
                               bool silent) noexcept TANH_NONBLOCKING_FUNCTION {
    m_src = src;
    m_play_id = id;
    if (silent) { return; }
    m_need_phase = true;
    m_ramp_restart = true;
    if (!m_last_live) { start_glide(); }
}

void MotionRecorder::select_source(const MotionLane& lane) noexcept TANH_NONBLOCKING_FUNCTION {
    const uint32_t lane_id = lane.m_take_id;
    for (uint32_t b = 0; b < 2; ++b) {
        TakeBuffer& buf = m_takes[b];
        if (buf.m_state.load(std::memory_order_acquire) != BufferState::Published) { continue; }
        const Source mine = b == 0 ? Source::Buffer0 : Source::Buffer1;
        if (m_src == mine) {
            // Same id: the lane is a bit-identical copy of this buffer.
            switch_to(Source::Lane, lane_id, lane_id == buf.m_take_id);
        }
        buf.m_state.store(BufferState::Free, std::memory_order_relaxed);
    }
    if (m_src == Source::Lane) {
        if (lane_id != m_play_id) { switch_to(Source::Lane, lane_id, false); }
    } else if (lane_id > m_play_id) {
        switch_to(Source::Lane, lane_id, false);  // a newer lane replaced the unpublished take
    }
}

void MotionRecorder::apply_commands(double bpm) noexcept TANH_NONBLOCKING_FUNCTION {
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
                if (m_take.m_active) {
                    if (m_take.m_bar) {
                        abort_take();
                    } else {
                        finish_take(bpm);
                    }
                }
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
                                const XYPadStream& in,
                                uint32_t offset,
                                double clock_beat) noexcept TANH_NONBLOCKING_FUNCTION {
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
    Take& k = m_take;
    k = Take{};

    if (bars > 0) {
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
    k.m_active = true;
    k.m_buffer = buffer;
    k.m_start_beat = clock_beat;
    const bool has_in = in.m_x != nullptr && in.m_y != nullptr && offset < in.m_num_samples;
    k.m_last_x = has_in ? in.m_x[offset] : m_last_x;
    k.m_last_y = has_in ? in.m_y[offset] : m_last_y;
    m_takes[buffer].m_state.store(BufferState::Recording, std::memory_order_relaxed);
    m_force_start = false;
    m_busy = false;
}

void MotionRecorder::write_point(float x,
                                 float y,
                                 uint8_t gate) noexcept TANH_NONBLOCKING_FUNCTION {
    Take& k = m_take;
    TakeBuffer& b = m_takes[k.m_buffer];
    const size_t l = k.m_count;
    const size_t cap = k.m_capacity;
    auto phys = [&](size_t logical) {
        const size_t p = k.m_start_index + logical;
        return p >= cap ? p - cap : p;
    };
    k.m_ring_x[l % 5] = x;
    k.m_ring_y[l % 5] = y;
    const size_t p = phys(l);
    b.m_x[p] = x;
    b.m_y[p] = y;
    b.m_gate[p] = gate;
    if (l >= 4) {
        // Centred 5-tap binomial [1 4 6 4 1] / 16, written two points behind.
        auto smooth = [&](const std::array<float, 5>& r) {
            const double v = (static_cast<double>(r[(l - 4) % 5]) + (4.0 * r[(l - 3) % 5]) +
                              (6.0 * r[(l - 2) % 5]) + (4.0 * r[(l - 1) % 5]) + r[l % 5]) /
                             16.0;
            return static_cast<float>(v);
        };
        const size_t q = phys(l - 2);
        b.m_x[q] = smooth(k.m_ring_x);
        b.m_y[q] = smooth(k.m_ring_y);
    }
    if (l == 0 || x != k.m_last_x || y != k.m_last_y) { k.m_count_at_change = l + 1; }
    k.m_count = l + 1;
    k.m_last_x = x;
    k.m_last_y = y;
}

void MotionRecorder::finalise(TakeBuffer& b,
                              size_t n,
                              double points_per_second) noexcept TANH_NONBLOCKING_FUNCTION {
    const Take& k = m_take;
    auto phys = [&](size_t logical) {
        const size_t p = k.m_start_index + logical;
        return p >= n ? p - n : p;
    };

    // The first two and the last two points are still raw (the running filter
    // writes two points behind). Seam blend first: spread the step at the wrap
    // beyond the expected velocity over the last W points (raised cosine), so
    // the looped curve is C1 at the seam.
    if (n >= k_min_take_points) {
        const auto window = static_cast<size_t>(
            std::max(1.0,
                     std::min(static_cast<double>(n / 4),
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

    // Then smooth the four edge points across the (now continuous) seam.
    if (n >= 8) {
        auto smooth_edges = [&](std::vector<float>& p) {
            // Logical points n-4 .. n-1, 0 .. 3 in a row.
            std::array<double, 8> v{};
            for (size_t j = 0; j < 8; ++j) { v[j] = p[phys((n - 4 + j) % n)]; }
            constexpr std::array<double, 5> w{1.0, 4.0, 6.0, 4.0, 1.0};
            for (size_t c = 2; c < 6; ++c) {  // n-2, n-1, 0, 1
                double acc = 0.0;
                for (size_t t = 0; t < 5; ++t) { acc += w[t] * v[c + t - 2]; }
                p[phys((n - 4 + c) % n)] = clamp01(acc / 16.0);
            }
        };
        smooth_edges(b.m_x);
        smooth_edges(b.m_y);
    }
}

void MotionRecorder::abort_take() noexcept TANH_NONBLOCKING_FUNCTION {
    if (!m_take.m_active) { return; }
    m_takes[m_take.m_buffer].m_state.store(BufferState::Free, std::memory_order_relaxed);
    m_take.m_active = false;
    m_armed = false;
    m_force_start = false;
}

void MotionRecorder::finish_take(double bpm,
                                 uint32_t trim_samples) noexcept TANH_NONBLOCKING_FUNCTION {
    Take& k = m_take;
    if (!k.m_active) { return; }
    if (!k.m_bar && trim_samples > 0 && k.m_count_at_change >= k_min_take_points) {
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
    const bool complete = k.m_bar ? k.m_count == k.m_capacity : k.m_count >= k_min_take_points;
    if (!complete) {
        abort_take();
        return;
    }
    TakeBuffer& b = m_takes[k.m_buffer];
    const size_t n = k.m_count;
    double points_per_second = 0.0;
    if (k.m_bar) {
        b.m_timebase = MotionTimebase::Beats;
        b.m_length = k.m_length;
        b.m_anchor = 0.0;
        b.m_rate = static_cast<double>(n) / k.m_length;
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
    switch_to(k.m_buffer == 0 ? Source::Buffer0 : Source::Buffer1, b.m_take_id, false);
}

void MotionRecorder::publish_snapshot(const LaneView& view) noexcept TANH_NONBLOCKING_FUNCTION {
    m_snap_has_lane = !view.empty();
    const double phase01 = view.empty() ? 0.0 : std::clamp(m_phase / view.m_length, 0.0, 1.0);
    double progress = 0.0;
    if (m_take.m_active && m_take.m_capacity > 0) {
        progress = static_cast<double>(m_take.m_count) / static_cast<double>(m_take.m_capacity);
    }
    uint64_t bits = static_cast<uint64_t>(state());
    bits |= static_cast<uint64_t>(m_play_enabled ? 1 : 0) << 4;
    bits |= static_cast<uint64_t>(m_reverse ? 1 : 0) << 5;
    bits |= static_cast<uint64_t>(view.m_timebase == MotionTimebase::Beats ? 1 : 0) << 6;
    bits |= static_cast<uint64_t>(m_busy ? 1 : 0) << 7;
    bits |= static_cast<uint64_t>(m_refused ? 1 : 0) << 8;
    bits |= static_cast<uint64_t>(m_aborted ? 1 : 0) << 9;
    bits |= static_cast<uint64_t>(std::lround(phase01 * 65535.0)) << 16;
    bits |= static_cast<uint64_t>(std::lround(std::clamp(progress, 0.0, 1.0) * 65535.0)) << 32;
    m_snap_bits.store(bits, std::memory_order_release);
    m_snap_take.store(view.empty() ? 0 : m_play_id, std::memory_order_relaxed);
    m_snap_length.store(view.empty() ? 0.0 : view.m_length, std::memory_order_relaxed);
}

void MotionRecorder::process(const TransportInfo& t,
                             const XYPadStream& in,
                             uint32_t num_samples) noexcept TANH_NONBLOCKING_FUNCTION {
    const auto n = static_cast<uint32_t>(std::min<size_t>(num_samples, m_max_block));
    m_num_samples = n;
    m_num_cps = 0;
    if (n == 0) { return; }

    // ── Clock: the transport while it plays, else a free-running (or held) beat.
    const double bpm = t.m_bpm > 0.0 ? t.m_bpm : 120.0;
    const double nominal = bpm / (60.0 * m_sample_rate);
    const bool playing = t.is_playing();
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

    apply_commands(bpm);

    const auto scope = m_lanes.read_scope(*m_audio_reader);
    const MotionLane& lane = scope.data();
    select_source(lane);

    LaneView view = view_of(m_src, lane);
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

    // ── Block-start phase: lane-phase test against the transport.
    m_seg_start = 0;
    if (!view.empty()) {
        dphase = direction(view);
        if (!m_need_phase) {
            if (view.m_timebase == MotionTimebase::Beats) {
                const double fresh = transport_phase(view, 0);
                const double d = circular_distance(fresh, m_phase, view.m_length);
                const double two_points =
                    2.0 * view.m_length / static_cast<double>(view.m_num_points);
                if (d <= k_keep_phase_eps) {
                    m_seg_phase = m_phase;
                } else if (d <= two_points) {
                    m_seg_phase = fresh;  // jitter or an aligned jump: no audible change
                } else {
                    // A real jump: never interpolate across it.
                    m_seg_phase = fresh;
                    m_ramp_restart = true;
                    if (!m_last_live) { start_glide(); }
                    ++m_jump_glides;
                }
            } else {
                m_seg_phase = m_phase;
            }
        }
    }

    auto relock = [&](uint32_t offset) {
        view = view_of(m_src, lane);
        m_need_phase = false;
        m_seg_start = offset;
        m_ramp_restart = true;
        if (view.empty()) {
            m_seg_phase = 0.0;
            return;
        }
        dphase = direction(view);
        m_seg_phase =
            view.m_timebase == MotionTimebase::Beats ? transport_phase(view, offset) : 0.0;
    };

    const uint32_t in_n =
        (in.m_x != nullptr && in.m_y != nullptr && in.m_active != nullptr) ? in.m_num_samples : 0;
    size_t in_cp = 0;
    const uint32_t interval = m_config.m_render_interval;
    const auto inv_interval = static_cast<float>(1.0 / interval);

    for (uint32_t i = 0; i < n; ++i) {
        const bool has_in = i < in_n;
        const bool live = has_in && in.m_active[i] != 0;
        const float lx = has_in ? in.m_x[i] : m_last_x;
        const float ly = has_in ? in.m_y[i] : m_last_y;
        bool cp = false;
        while (in_cp < in.m_change_points.size() && in.m_change_points[in_cp] <= i) {
            cp = cp || in.m_change_points[in_cp] == i;
            ++in_cp;
        }

        // ── Recording.
        if (!m_take.m_active && m_armed && (live || m_force_start)) {
            start_take(t, in, i, b0 + (static_cast<double>(i) * slope));
        }
        if (m_take.m_active) {
            Take& k = m_take;
            if (!k.m_bar && k.m_saw_touch && !live) {
                finish_take(bpm, n);  // free take: ends on release
            } else {
                k.m_saw_touch = k.m_saw_touch || live;
                const double next =
                    k.m_tick_base + (static_cast<double>(k.m_count) * k.m_tick_step);
                if (k.m_clock >= next - k_tick_eps) {
                    if (live) {
                        write_point(lx, ly, 1);
                    } else {
                        write_point(k.m_last_x, k.m_last_y, 0);
                    }
                    if (k.m_count >= k.m_capacity) { finish_take(bpm); }
                }
                k.m_clock += k.m_beats ? rec_slope : 1.0;
            }
        }

        if (m_need_phase) {
            relock(i);
            cp = true;
        }

        // ── Playback.
        const bool pb_on = m_play_enabled && !view.empty();
        float px = m_last_x;
        float py = m_last_y;
        uint8_t pg = 0;
        if (pb_on) {
            const auto np = static_cast<double>(view.m_num_points);
            const double scale = np / view.m_length;
            const double k = static_cast<double>(i - m_seg_start);
            double p = m_seg_phase + (k * dphase);
            if (p < 0.0 || p >= view.m_length) { p = detail::wrap_phase(p, view.m_length); }
            auto gi = static_cast<size_t>(p * scale);
            if (gi >= view.m_num_points) { gi = view.m_num_points - 1; }
            pg = view.m_gate[gi];
            // The render grid restarts at every loop wrap, so each loop is
            // rendered on the same ticks (loop n == loop 1).
            const double half = 0.5 * view.m_length;
            const bool wrapped = dphase > 0.0 ? p < m_last_p - half : p > m_last_p + half;
            m_last_p = p;
            if (wrapped) { m_ramp_restart = true; }
            if (m_ramp_restart || m_ramp_pos >= interval) {
                if (m_ramp_restart) {
                    m_ramp_to_x = detail::catmull_rom_wrap(view.m_x, view.m_num_points, p * scale);
                    m_ramp_to_y = detail::catmull_rom_wrap(view.m_y, view.m_num_points, p * scale);
                    m_ramp_restart = false;
                }
                m_ramp_from_x = m_ramp_to_x;
                m_ramp_from_y = m_ramp_to_y;
                double ahead = m_seg_phase + ((k + interval) * dphase);
                if (ahead < 0.0 || ahead >= view.m_length) {
                    ahead = detail::wrap_phase(ahead, view.m_length);
                }
                m_ramp_to_x = detail::catmull_rom_wrap(view.m_x, view.m_num_points, ahead * scale);
                m_ramp_to_y = detail::catmull_rom_wrap(view.m_y, view.m_num_points, ahead * scale);
                m_ramp_pos = 0;
                cp = true;
            }
            const float w = static_cast<float>(m_ramp_pos) * inv_interval;
            px = m_ramp_from_x + ((m_ramp_to_x - m_ramp_from_x) * w);
            py = m_ramp_from_y + ((m_ramp_to_y - m_ramp_from_y) * w);
            ++m_ramp_pos;
        }

        // ── Compose: live touch > playback (with glides) > hold.
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
        if (cp && (m_num_cps == 0 || m_cps[m_num_cps - 1] != i)) { m_cps[m_num_cps++] = i; }
        m_last_x = ox;
        m_last_y = oy;
        m_last_gate = og;
        m_last_live = live;
    }

    if (!view.empty()) {
        m_phase = detail::wrap_phase(m_seg_phase + (static_cast<double>(n - m_seg_start) * dphase),
                                     view.m_length);
    } else {
        m_phase = 0.0;
    }
    m_clock_end = b0 + (static_cast<double>(n) * slope);
    publish_snapshot(view);
}

}  // namespace thl::modulation
