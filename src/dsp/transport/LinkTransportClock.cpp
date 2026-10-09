#include "tanh/dsp/transport/LinkTransportClock.h"

#include <tanh/dsp/transport/LinkBackend.h>
#include <tanh/dsp/transport/TransportInfo.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <atomic>
#include <cmath>
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
// How long after an epoch change (Link enabled, first peer joined) a phase
// realignment still counts as the join. Link publishes a joined timeline to the
// audio thread up to one second late while a local change is fresh
// (kLocalModGracePeriod), so the realignment can trail the epoch by that much.
constexpr int64_t k_epoch_window_us = 1500000;
}  // namespace

LinkTransportClock::LinkTransportClock(LinkBackend& backend) : m_backend(backend) {}

void LinkTransportClock::on_prepare(double sample_rate) {
    if (sample_rate > 0.0) { m_sample_rate = sample_rate; }
    m_tracker.prepare(m_sample_rate);
    m_tracker.set_tolerance_samples(
        std::max(2.0, k_jitter_tolerance_us * m_sample_rate / k_micros_per_second));
    m_tempo_window_samples = k_peer_tempo_window_us * m_sample_rate / k_micros_per_second;
    m_sample_time = 0.0;
    m_host_time_us.reset();
    m_host_time_filter.prepare(m_sample_rate);
    m_seen_epoch = m_backend.timeline_epoch();
    m_epoch_armed = false;
}

int64_t LinkTransportClock::latency_us(uint32_t samples, double sample_rate) {
    if (!(sample_rate > 0.0)) { return 0; }
    return std::llround(static_cast<double>(samples) * k_micros_per_second / sample_rate);
}

void LinkTransportClock::set_output_latency_samples(uint32_t samples) {
    m_latency_samples.store(samples, std::memory_order_relaxed);
}

int64_t LinkTransportClock::output_time(uint32_t frame_count) TANH_NONBLOCKING_FUNCTION {
    // Output time t0: the samples of this block leave the speaker at t0.
    const int64_t host_us = m_host_time_us.has_value()
                                ? *m_host_time_us
                                : m_host_time_filter.add(m_sample_time, m_backend.now_us());
    m_host_time_us.reset();
    m_sample_time += static_cast<double>(frame_count);
    return host_us + latency_us(m_latency_samples.load(std::memory_order_relaxed), m_sample_rate);
}

void LinkTransportClock::follow_epoch(int64_t t0) TANH_NONBLOCKING_FUNCTION {
    // An epoch change arms the reset for the next realignment within the window:
    // Link may publish the joined timeline blocks after the epoch moved, and
    // enabling Link without peers moves nothing (Link TEST-PLAN BEATTIME-1). The
    // epoch is read before capture(), so a timeline published before the bump is
    // in this capture. Link publishes a joined timeline just before it counts the
    // new peer; a block captured in between reports that realignment as k_jumped
    // only.
    const uint64_t epoch = m_backend.timeline_epoch();
    if (epoch != m_seen_epoch) {
        m_seen_epoch = epoch;
        m_epoch_armed = true;
        m_epoch_armed_until_us = t0 + k_epoch_window_us;
    }
    if (m_epoch_armed && t0 >= m_epoch_armed_until_us) { m_epoch_armed = false; }
}

LinkState LinkTransportClock::apply_requests(int64_t t0, double quantum) TANH_NONBLOCKING_FUNCTION {
    LinkState state = m_backend.state();
    bool dirty = false;
    if (const auto bpm = m_pending_bpm.take()) {
        m_backend.set_tempo(*bpm, t0);
        dirty = true;
    }
    const int play_request = m_pending_play.exchange(0, std::memory_order_acq_rel);
    if (play_request > 0 && !state.m_playing) {
        m_backend.set_playing(true, t0);
        m_backend.request_beat_at_start(0.0, quantum);  // quantized launch
        dirty = true;
    } else if (play_request < 0 && state.m_playing) {
        m_backend.set_playing(false, t0);
        dirty = true;
    }
    if (const auto beats = m_pending_position.take()) {
        m_backend.request_beat_at(*beats, t0, quantum);
        dirty = true;
    }
    if (!dirty) { return state; }
    m_backend.commit();
    return m_backend.state();
}

TransportInfo LinkTransportClock::compute_block(uint32_t frame_count) {
    const double q = m_backend.quantum();
    const int64_t t0 = output_time(frame_count);
    m_t0_us = t0;
    follow_epoch(t0);

    // Capture, apply queued UI requests at the output time, commit if changed.
    m_backend.capture();
    const LinkState state = apply_requests(t0, q);

    const int64_t t1 =
        t0 + std::llround(static_cast<double>(frame_count) * k_micros_per_second / m_sample_rate);
    const double b0 = m_backend.beat_at(t0, q);
    const double b1 = m_backend.beat_at(t1, q);
    // A start or stop dated ahead takes effect in the block that contains it.
    const bool playing =
        state.m_playing ? state.m_play_time_us < t1 : is_playing() && state.m_play_time_us >= t1;

    TransportInfo info;
    info.m_flags = TransportInfo::k_has_tempo | TransportInfo::k_has_beat_position |
                   TransportInfo::k_has_time_signature;
    if (playing) { info.m_flags |= TransportInfo::k_is_playing; }
    info.m_bpm = state.m_tempo;
    const auto sig = m_time_signature.load();
    info.m_sig_num = sig.m_num;
    info.m_sig_denom = sig.m_denom;
    info.m_beat_position = b0;

    // While armed a join's realignment must not be absorbed as a peer's tempo
    // change, so the tempo window is off.
    m_tracker.set_tempo_window_samples(m_epoch_armed ? 0.0 : m_tempo_window_samples);
    m_tracker.resolve(info, b1, frame_count);
    if (m_epoch_armed && info.has(TransportInfo::k_jumped)) {
        info.m_flags |= TransportInfo::k_timeline_reset;
        m_epoch_armed = false;
    }
    return info;
}

void LinkTransportClock::set_bpm(double bpm) {
    m_pending_bpm.request(std::max(bpm, k_min_bpm));
}

void LinkTransportClock::set_time_signature(int num, int denom) {
    m_time_signature.store(num, denom);
}

void LinkTransportClock::play() {
    m_pending_play.store(1, std::memory_order_release);
}

void LinkTransportClock::stop() {
    m_pending_play.store(-1, std::memory_order_release);
}

void LinkTransportClock::set_position_beats(double beats) {
    m_pending_position.request(beats);
}

}  // namespace thl::dsp::transport
