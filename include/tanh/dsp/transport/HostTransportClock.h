#pragma once

#include <tanh/dsp/transport/TransportClock.h>
#include <tanh/dsp/transport/TransportInfo.h>

#include <atomic>
#include <cstdint>
#include <optional>

namespace thl::dsp::transport {

/**
 * @brief TransportClock fed by a plugin host's playhead, with fallbacks.
 *
 * Each block the audio thread converts the host's position (JUCE PositionInfo,
 * CLAP transport, VST3 ProcessContext) into a TransportInfo with validity flags
 * and passes it to set_host_info() before begin_block(). Fields the host omits
 * come from the fallbacks set with set_bpm(), set_time_signature(), play(),
 * stop() and set_position_beats():
 *
 * - Tempo: the host's if valid, else the fallback.
 * - Play state: the host's if it sent tempo, beat or time signature, else the
 *   fallback.
 * - Beat: the host's if valid, else free-running from the previous block's end.
 *   set_position_beats() moves the free-running beat; a seek made while the host
 *   supplies the beat stays pending until the beat free-runs again.
 * - Time signature: the host's if valid, else the fallback.
 *
 * Discontinuity flags come from a ContinuityTracker, never from the host. A block
 * without a set_host_info() call has no host data.
 *
 * set_host_info(), begin_block(), end_block() and the getters are real-time
 * safe. The fallback setters are lock-free and callable from any thread.
 */
class TANH_API HostTransportClock final : public TransportClock {
public:
    // ── Main thread ──────────────────────────────────────────────────────────
    void prepare(double sample_rate) override;

    // ── Audio thread only ─────────────────────────────────────────────────────

    /// The host's view of the next block, consumed by begin_block(). Discontinuity
    /// bits in @p info are ignored.
    void set_host_info(const TransportInfo& info) TANH_NONBLOCKING_FUNCTION;

    void begin_block(uint32_t frame_count,
                     std::optional<int64_t> host_time_micros = std::nullopt) override;
    void end_block() override;

    [[nodiscard]] double beat_at_sample(uint32_t offset) const override;
    [[nodiscard]] bool division_in_block(Division div) const override;
    [[nodiscard]] bool is_playing() const override;
    [[nodiscard]] double bpm() const override;
    [[nodiscard]] int sig_num() const override;
    [[nodiscard]] int sig_denom() const override;
    /// Samples processed while playing since prepare().
    [[nodiscard]] uint64_t sample_position() const override;
    [[nodiscard]] TransportInfo block_info() const override;

    // ── Any thread — lock-free fallbacks for fields the host omits ────────────
    void set_bpm(double bpm) override;
    void set_time_signature(int num, int denom) override;
    void play() override;
    void stop() override;
    void set_position_beats(double beats) override;

private:
    double m_sample_rate = 48000.0;
    uint64_t m_sample_position = 0;

    TransportInfo m_host{};
    TransportInfo m_info{};
    ContinuityTracker m_tracker;

    // Free-run: beat = anchor + samples * bpm / (60 * sample rate).
    double m_free_anchor_beat = 0.0;
    double m_free_bpm = 0.0;
    uint64_t m_free_samples = 0;
    bool m_free_valid = false;  // false: re-anchor at the previous block's end

    std::atomic<double> m_fallback_bpm{120.0};
    std::atomic<int> m_fallback_sig_num{4};
    std::atomic<int> m_fallback_sig_denom{4};
    std::atomic<bool> m_fallback_playing{false};
    std::atomic<double> m_pending_position_beats{0.0};
    std::atomic<bool> m_has_pending_position{false};
};

}  // namespace thl::dsp::transport
