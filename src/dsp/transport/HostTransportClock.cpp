#include "tanh/dsp/transport/HostTransportClock.h"

#include <tanh/dsp/transport/TransportInfo.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <optional>

namespace thl::dsp::transport {

namespace {

// A host that sent none of these has no transport (a plain effect host, an
// offline render): its play flag means nothing and the fallback applies.
constexpr uint32_t k_musical_fields = TransportInfo::k_has_tempo |
                                      TransportInfo::k_has_beat_position |
                                      TransportInfo::k_has_time_signature;
constexpr double k_min_bpm = 1.0;

}  // namespace

void HostTransportClock::prepare(double sample_rate) {
    if (sample_rate > 0.0) { m_sample_rate = sample_rate; }
    m_tracker.prepare(m_sample_rate);
    m_sample_position = 0;
    m_free_valid = false;
    m_host.m_flags = 0;
}

void HostTransportClock::set_host_info(const TransportInfo& info) TANH_NONBLOCKING_FUNCTION {
    m_host = info;
    m_host.m_flags &= ~TransportInfo::k_discontinuity_mask;
}

void HostTransportClock::begin_block(uint32_t frame_count,
                                     std::optional<int64_t> /*host_time_micros*/) {
    const TransportInfo host = m_host;
    m_host.m_flags = 0;  // consumed: the next block without set_host_info() has no host data

    const bool host_musical = (host.m_flags & k_musical_fields) != 0;
    const bool playing =
        host_musical ? host.is_playing() : m_fallback_playing.load(std::memory_order_acquire);

    double bpm = m_fallback_bpm.load(std::memory_order_acquire);
    if (host.has(TransportInfo::k_has_tempo) && host.m_bpm >= k_min_bpm) { bpm = host.m_bpm; }

    int num = m_fallback_sig_num.load(std::memory_order_acquire);
    int den = m_fallback_sig_denom.load(std::memory_order_acquire);
    if (host.has(TransportInfo::k_has_time_signature) && host.m_sig_num > 0 &&
        host.m_sig_denom > 0) {
        num = host.m_sig_num;
        den = host.m_sig_denom;
    }

    const double bps = bpm / (60.0 * m_sample_rate);

    double start = 0.0;
    double end = 0.0;
    if (host.has(TransportInfo::k_has_beat_position)) {
        start = host.m_beat_position;
        // Predicted with the block-start tempo (all a per-block host reports). An
        // in-block tempo change is absorbed by the tracker at the next block.
        end = playing ? start + static_cast<double>(frame_count) * bps : start;
        m_free_valid = false;  // a later free-run continues from this block's end
    } else {
        // A seek is taken only here, so one made while the host supplied the beat
        // waits for the free-run instead of being dropped.
        if (m_has_pending_position.exchange(false, std::memory_order_acq_rel)) {
            m_free_anchor_beat = m_pending_position_beats.load(std::memory_order_acquire);
            m_free_samples = 0;
            m_free_bpm = bpm;
            m_free_valid = true;
        } else if (!m_free_valid || bpm != m_free_bpm) {
            m_free_anchor_beat = m_info.beat_end();
            m_free_samples = 0;
            m_free_bpm = bpm;
            m_free_valid = true;
        }
        start = m_free_anchor_beat + static_cast<double>(m_free_samples) * bps;
        end = playing ? m_free_anchor_beat + static_cast<double>(m_free_samples + frame_count) * bps
                      : start;
    }

    TransportInfo info;
    info.m_flags = TransportInfo::k_has_tempo | TransportInfo::k_has_beat_position |
                   TransportInfo::k_has_time_signature;
    if (playing) { info.m_flags |= TransportInfo::k_is_playing; }
    info.m_bpm = bpm;
    info.m_sig_num = num;
    info.m_sig_denom = den;
    info.m_beat_position = start;

    m_tracker.resolve(info, end, frame_count);
    m_info = info;
}

void HostTransportClock::end_block() {
    if (!m_info.is_playing()) { return; }
    m_sample_position += m_info.m_num_samples;
    if (m_free_valid) { m_free_samples += m_info.m_num_samples; }
}

double HostTransportClock::beat_at_sample(uint32_t offset) const {
    return m_info.beat_at(offset);
}

bool HostTransportClock::division_in_block(Division div) const {
    return m_info.is_playing() && m_info.division_in_block(div);
}

bool HostTransportClock::is_playing() const {
    return m_info.is_playing();
}

double HostTransportClock::bpm() const {
    return m_info.m_bpm;
}

int HostTransportClock::sig_num() const {
    return m_info.m_sig_num;
}

int HostTransportClock::sig_denom() const {
    return m_info.m_sig_denom;
}

uint64_t HostTransportClock::sample_position() const {
    return m_sample_position;
}

TransportInfo HostTransportClock::block_info() const {
    return m_info;
}

void HostTransportClock::set_bpm(double bpm) {
    m_fallback_bpm.store(std::max(bpm, k_min_bpm), std::memory_order_release);
}

void HostTransportClock::set_time_signature(int num, int denom) {
    if (num <= 0 || denom <= 0) { return; }
    m_fallback_sig_num.store(num, std::memory_order_release);
    m_fallback_sig_denom.store(denom, std::memory_order_release);
}

void HostTransportClock::play() {
    m_fallback_playing.store(true, std::memory_order_release);
}

void HostTransportClock::stop() {
    m_fallback_playing.store(false, std::memory_order_release);
}

void HostTransportClock::set_position_beats(double beats) {
    m_pending_position_beats.store(beats, std::memory_order_release);
    m_has_pending_position.store(true, std::memory_order_release);
}

}  // namespace thl::dsp::transport
