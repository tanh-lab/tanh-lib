#pragma once

#include <atomic>
#include <optional>

#include <tanh/transport/TransportClock.h>

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
 * Drop-in replaceable with a future LinkTransportClock via the
 * thl::TransportClock interface.
 *
 * @section rt_safety Real-Time Safety
 *   begin_block() and end_block() are real-time safe (no allocation, no locks).
 *   beat_at_sample() is real-time safe.
 *   All setters are lock-free.
 */
namespace thl {

class TANH_API InternalTransportClock final : public TransportClock {
public:
    // ── Main thread ──────────────────────────────────────────────────────────
    void prepare(double sample_rate) override;

    // ── Audio thread only ─────────────────────────────────────────────────────
    void begin_block(uint32_t frame_count,
                     std::optional<int64_t> host_time_micros = std::nullopt) override;
    void end_block() override;

    [[nodiscard]] double beat_at_sample(uint32_t offset) const override;
    [[nodiscard]] bool division_in_block(Division div) const override;
    [[nodiscard]] bool is_playing() const override;
    [[nodiscard]] double bpm() const override;
    [[nodiscard]] int sig_num() const override;
    [[nodiscard]] uint64_t sample_position() const override;

    // ── Any thread — lock-free ────────────────────────────────────────────────
    void set_bpm(double bpm) override;
    void set_time_signature(int num, int denom) override;
    void play() override;
    void stop() override;
    void set_position_beats(double beats) override;

private:
    // Audio thread only — never read or written from other threads
    uint64_t m_sample_position = 0;
    uint32_t m_frame_count = 0;
    double m_sample_rate = 48000.0;

    // Latched at begin_block — stable for the full block duration
    double m_active_bpm = 120.0;
    double m_active_beats_per_sample = 0.0;  // bpm / (60 * sample_rate), cached
    int m_active_sig_num = 4;
    int m_active_sig_denom = 4;
    bool m_active_playing = false;

    // Written from any thread, read only inside begin_block
    std::atomic<double> m_pending_bpm{120.0};
    std::atomic<int> m_pending_sig_num{4};
    std::atomic<int> m_pending_sig_denom{4};
    std::atomic<bool> m_playing{false};

    // Seek — position written before flag to ensure visibility ordering
    std::atomic<double> m_pending_position_beats{0.0};
    std::atomic<bool> m_has_pending_position{false};
};

}  // namespace thl
