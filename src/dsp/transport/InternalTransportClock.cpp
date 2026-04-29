#include <tanh/dsp/transport/InternalTransportClock.h>
#include <tanh/dsp/transport/TransportClock.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <optional>

namespace thl::dsp::transport {

// ── Main thread ───────────────────────────────────────────────────────────────

void InternalTransportClock::prepare(double sample_rate) {
    m_sample_rate = sample_rate;
}

// ── Audio thread ──────────────────────────────────────────────────────────────

void InternalTransportClock::begin_block(uint32_t frame_count,
                                         std::optional<int64_t> /*host_time_micros*/) {
    m_frame_count = frame_count;

    // Latch — changes from any thread take effect here, never mid-block
    m_active_playing = m_playing.load(std::memory_order_acquire);
    m_active_bpm = m_pending_bpm.load(std::memory_order_acquire);
    m_active_sig_num = m_pending_sig_num.load(std::memory_order_acquire);
    m_active_sig_denom = m_pending_sig_denom.load(std::memory_order_acquire);

    m_active_beats_per_sample = m_active_bpm / (60.0 * m_sample_rate);

    // Seek — flag read last so position is visible before we act on it
    if (m_has_pending_position.exchange(false, std::memory_order_acq_rel)) {
        const double beats = m_pending_position_beats.load(std::memory_order_acquire);
        const double clamped = std::max(0.0, beats);
        m_sample_position = static_cast<uint64_t>(clamped * 60.0 * m_sample_rate / m_active_bpm);
    }
}

void InternalTransportClock::end_block() {
    if (m_active_playing) { m_sample_position += m_frame_count; }
}

double InternalTransportClock::beat_at_sample(uint32_t offset) const {
    if (!m_active_playing) {
        return static_cast<double>(m_sample_position) * m_active_beats_per_sample;
    }
    return static_cast<double>(m_sample_position + offset) * m_active_beats_per_sample;
}

bool InternalTransportClock::division_in_block(Division div) const {
    if (!m_active_playing) { return false; }

    const double size = beats_per_division(div, m_active_sig_num, m_active_sig_denom);
    const double start = beat_at_sample(0);
    const double end = beat_at_sample(m_frame_count);
    // Half-open [start, end): a boundary at offset = frame_count belongs to
    // the next block, not this one.
    const double next = std::ceil(start / size) * size;
    return next < end;
}

bool InternalTransportClock::is_playing() const {
    return m_active_playing;
}

double InternalTransportClock::bpm() const {
    return m_active_bpm;
}

int InternalTransportClock::sig_num() const {
    return m_active_sig_num;
}

int InternalTransportClock::sig_denom() const {
    return m_active_sig_denom;
}

uint64_t InternalTransportClock::sample_position() const {
    return m_sample_position;
}

// ── Any thread ────────────────────────────────────────────────────────────────

void InternalTransportClock::set_bpm(double bpm) {
    constexpr double k_min_bpm = 1.0;
    m_pending_bpm.store(std::max(bpm, k_min_bpm), std::memory_order_release);
}

void InternalTransportClock::set_time_signature(int num, int denom) {
    m_pending_sig_num.store(num, std::memory_order_release);
    m_pending_sig_denom.store(denom, std::memory_order_release);
}

void InternalTransportClock::play() {
    m_playing.store(true, std::memory_order_release);
}

void InternalTransportClock::stop() {
    m_playing.store(false, std::memory_order_release);
}

void InternalTransportClock::set_position_beats(double beats) {
    m_pending_position_beats.store(beats, std::memory_order_release);
    // Flag written last — position must be visible before the audio thread acts on it
    m_has_pending_position.store(true, std::memory_order_release);
}

}  // namespace thl::dsp::transport
