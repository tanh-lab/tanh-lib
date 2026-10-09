#pragma once

#include <tanh/dsp/transport/ContinuityTracker.h>
#include <tanh/dsp/transport/TransportClock.h>
#include <tanh/dsp/transport/detail/ClockRequests.h>

#include <atomic>
#include <cstdint>

namespace thl::dsp::transport {

/**
 * @class InternalTransportClock
 * @brief Standalone transport clock driven by the audio callback.
 *
 * A uint64_t sample counter is the authoritative time source.
 * Beat position is derived on demand — no floating-point accumulation,
 * no drift over long sessions.
 *
 * State changes from any thread are buffered in atomics and latched
 * at buffer boundaries. No locks in the audio path.
 *
 * A tempo change keeps the beat continuous. block_info() reports seeks as
 * k_jumped, play/stop as k_started/k_stopped, tempo changes as k_tempo_changed
 * and the first block after prepare() as k_timeline_reset. prepare() keeps the
 * beat, also at a new sample rate.
 *
 * Drop-in replaceable with HostTransportClock or LinkTransportClock via the
 * TransportClock interface.
 *
 * @par Real-Time Safety
 *   begin_block() and end_block() are real-time safe (no allocation, no locks).
 *   beat_at_sample() is real-time safe.
 *   All setters are lock-free.
 */
class TANH_API InternalTransportClock final : public TransportClock {
public:
    // ── Any thread — lock-free ────────────────────────────────────────────────
    void set_bpm(double bpm) override;
    void set_time_signature(int num, int denom) override;
    void play() override;
    void stop() override;
    void set_position_beats(double beats) override;

protected:
    void on_prepare(double sample_rate) override;
    [[nodiscard]] TransportInfo compute_block(uint32_t frame_count) override;

private:
    [[nodiscard]] double raw_beat(uint32_t offset) const;

    // Audio thread only — never read or written from other threads.
    // Timeline position in samples (moved by seeks); advances while playing.
    uint64_t m_timeline_samples = 0;
    uint32_t m_advance = 0;  // samples the previous block played, added at the next block
    double m_sample_rate = 48000.0;

    // Latched at begin_block — stable for the full block duration
    double m_active_bpm = 120.0;
    double m_active_beats_per_sample = 0.0;  // bpm / (60 * sample_rate), cached
    bool m_active_playing = false;

    // Beat = m_anchor_beat + (m_timeline_samples - m_anchor_sample) * beats/sample.
    // Re-anchored on tempo changes and seeks so the beat stays continuous.
    double m_anchor_beat = 0.0;
    uint64_t m_anchor_sample = 0;

    ContinuityTracker m_tracker;

    // Written from any thread, read only inside begin_block
    std::atomic<double> m_pending_bpm{120.0};
    detail::AtomicTimeSignature m_time_signature;
    std::atomic<bool> m_playing{false};
    detail::PendingValue m_pending_position;
};

}  // namespace thl::dsp::transport
