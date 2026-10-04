#include <tanh/dsp/transport/LinkBackend.h>
#include <tanh/dsp/transport/LinkTransportClock.h>
#include <tanh/dsp/transport/TransportInfo.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace thl::dsp::transport {

namespace {
constexpr double k_min_bpm = 1.0;
constexpr double k_micros_per_second = 1e6;
// Host-time jitter the tracker absorbs (two blocks' jitter add up in the
// start-vs-expected difference). A Link phase realignment moves the beat by a
// fraction of the quantum, orders of magnitude more.
constexpr double k_jitter_tolerance_us = 500.0;
// Peers apply tempo changes at their output time (LinkHut: callback host time +
// output latency). The block that first sees a change can start after that time
// (network and thread delays) or before it (the peer's latency exceeds ours),
// and Link's timeline is one line, so the new tempo also applies before the
// change time. Changes dated within ±50 ms of the block start are tempo changes.
constexpr double k_peer_tempo_window_us = 50000.0;
}  // namespace

LinkTransportClock::LinkTransportClock(LinkBackend& backend) : m_backend(backend) {}

void LinkTransportClock::prepare(double sample_rate) {
    if (sample_rate > 0.0) { m_sample_rate = sample_rate; }
    m_tracker.prepare(m_sample_rate);
    m_tracker.set_tolerance_samples(
        std::max(2.0, k_jitter_tolerance_us * m_sample_rate / k_micros_per_second));
    m_tempo_window_samples = k_peer_tempo_window_us * m_sample_rate / k_micros_per_second;
    m_info = TransportInfo{};
    m_sample_position = 0;
    m_filter_count = 0;
    m_filter_index = 0;
    m_seen_epoch = m_backend.timeline_epoch();
}

int64_t LinkTransportClock::latency_us(uint32_t samples, double sample_rate) noexcept {
    if (!(sample_rate > 0.0)) { return 0; }
    return std::llround(static_cast<double>(samples) * k_micros_per_second / sample_rate);
}

void LinkTransportClock::set_output_latency_samples(uint32_t samples) noexcept {
    m_latency_samples.store(samples, std::memory_order_relaxed);
}

void LinkTransportClock::set_quantum(double beats) noexcept {
    if (beats > 0.0) { m_quantum.store(beats, std::memory_order_relaxed); }
}

double LinkTransportClock::quantum() const noexcept {
    return m_quantum.load(std::memory_order_relaxed);
}

int64_t LinkTransportClock::filtered_host_time_us(double sample_time) noexcept
    TANH_NONBLOCKING_FUNCTION {
    const auto now = static_cast<double>(m_backend.now_us());
    if (m_filter_count == 0) {
        m_filter_x0 = sample_time;
        m_filter_y0 = now;
    }
    m_filter_x[m_filter_index] = sample_time - m_filter_x0;
    m_filter_y[m_filter_index] = now - m_filter_y0;
    m_filter_index = (m_filter_index + 1) % k_filter_points;
    m_filter_count = std::min(m_filter_count + 1, k_filter_points);

    const auto n = static_cast<double>(m_filter_count);
    double mean_x = 0.0;
    double mean_y = 0.0;
    for (size_t i = 0; i < m_filter_count; ++i) {
        mean_x += m_filter_x[i];
        mean_y += m_filter_y[i];
    }
    mean_x /= n;
    mean_y /= n;
    double sxy = 0.0;
    double sxx = 0.0;
    for (size_t i = 0; i < m_filter_count; ++i) {
        const double dx = m_filter_x[i] - mean_x;
        sxy += dx * (m_filter_y[i] - mean_y);
        sxx += dx * dx;
    }
    const double slope = sxx > 0.0 ? sxy / sxx : k_micros_per_second / m_sample_rate;
    const double intercept = mean_y - (slope * mean_x);
    return std::llround(m_filter_y0 + intercept + (slope * (sample_time - m_filter_x0)));
}

void LinkTransportClock::begin_block(uint32_t frame_count,
                                     std::optional<int64_t> host_time_micros) {
    const double q = m_quantum.load(std::memory_order_relaxed);
    const int64_t host_us = host_time_micros.has_value()
                                ? *host_time_micros
                                : filtered_host_time_us(static_cast<double>(m_sample_position));
    const int64_t t0 =
        host_us + latency_us(m_latency_samples.load(std::memory_order_relaxed), m_sample_rate);
    m_t0_us = t0;

    // Capture, apply queued UI requests at the output time, commit if changed.
    m_backend.capture();
    LinkState state = m_backend.state();
    bool dirty = false;
    if (m_has_pending_bpm.exchange(false, std::memory_order_acq_rel)) {
        m_backend.set_tempo(m_pending_bpm.load(std::memory_order_acquire), t0);
        dirty = true;
    }
    const int play_request = m_pending_play.exchange(0, std::memory_order_acq_rel);
    if (play_request > 0 && !state.m_playing) {
        m_backend.set_playing(true, t0);
        m_backend.request_beat_at_start(0.0, q);  // quantized launch
        dirty = true;
    } else if (play_request < 0 && state.m_playing) {
        m_backend.set_playing(false, t0);
        dirty = true;
    }
    if (m_has_pending_position.exchange(false, std::memory_order_acq_rel)) {
        m_backend.request_beat_at(m_pending_position_beats.load(std::memory_order_acquire), t0, q);
        dirty = true;
    }
    if (dirty) {
        m_backend.commit();
        state = m_backend.state();
    }

    const int64_t t1 =
        t0 + std::llround(static_cast<double>(frame_count) * k_micros_per_second / m_sample_rate);
    const double b0 = m_backend.beat_at(t0, q);
    const double b1 = m_backend.beat_at(t1, q);
    // A start or stop dated ahead takes effect in the block that contains it.
    const bool playing = state.m_playing ? state.m_play_time_us < t1
                                         : m_info.is_playing() && state.m_play_time_us >= t1;

    TransportInfo info;
    info.m_flags = TransportInfo::k_has_tempo | TransportInfo::k_has_beat_position |
                   TransportInfo::k_has_time_signature | TransportInfo::k_has_host_time;
    if (playing) { info.m_flags |= TransportInfo::k_is_playing; }
    info.m_bpm = state.m_tempo;
    info.m_sig_num = m_sig_num.load(std::memory_order_relaxed);
    info.m_sig_denom = m_sig_denom.load(std::memory_order_relaxed);
    info.m_quantum = q;
    info.m_host_time_ns = host_us * 1000;
    info.m_beat_position = b0;

    const uint64_t epoch = m_backend.timeline_epoch();
    const bool epoch_changed = epoch != m_seen_epoch;
    m_seen_epoch = epoch;

    // A join realigns the phase: never absorb that as a peer's tempo change.
    m_tracker.set_tempo_window_samples(epoch_changed ? 0.0 : m_tempo_window_samples);
    m_tracker.resolve(info, b1, frame_count);
    if (epoch_changed && info.has(TransportInfo::k_jumped)) {
        info.m_flags |= TransportInfo::k_timeline_reset;
    }
    m_info = info;
}

void LinkTransportClock::end_block() {
    m_sample_position += m_info.m_num_samples;
}

double LinkTransportClock::beat_at_sample(uint32_t offset) const {
    return m_info.beat_at(offset);
}

bool LinkTransportClock::division_in_block(Division div) const {
    return m_info.is_playing() && m_info.division_in_block(div);
}

bool LinkTransportClock::is_playing() const {
    return m_info.is_playing();
}

double LinkTransportClock::bpm() const {
    return m_info.m_bpm;
}

int LinkTransportClock::sig_num() const {
    return m_info.m_sig_num;
}

int LinkTransportClock::sig_denom() const {
    return m_info.m_sig_denom;
}

uint64_t LinkTransportClock::sample_position() const {
    return m_sample_position;
}

TransportInfo LinkTransportClock::block_info() const {
    return m_info;
}

uint32_t LinkTransportClock::discontinuities() const {
    return m_info.discontinuities();
}

void LinkTransportClock::set_bpm(double bpm) {
    m_pending_bpm.store(std::max(bpm, k_min_bpm), std::memory_order_release);
    m_has_pending_bpm.store(true, std::memory_order_release);
}

void LinkTransportClock::set_time_signature(int num, int denom) {
    if (num <= 0 || denom <= 0) { return; }
    m_sig_num.store(num, std::memory_order_relaxed);
    m_sig_denom.store(denom, std::memory_order_relaxed);
}

void LinkTransportClock::play() {
    m_pending_play.store(1, std::memory_order_release);
}

void LinkTransportClock::stop() {
    m_pending_play.store(-1, std::memory_order_release);
}

void LinkTransportClock::set_position_beats(double beats) {
    m_pending_position_beats.store(beats, std::memory_order_release);
    m_has_pending_position.store(true, std::memory_order_release);
}

}  // namespace thl::dsp::transport
