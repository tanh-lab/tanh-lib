#pragma once

#include <tanh/dsp/transport/TransportClock.h>
#include <tanh/dsp/transport/TransportInfo.h>

#include <atomic>
#include <cstdint>
#include <optional>

namespace thl::dsp::transport {

/**
 * @class HostTransportClock
 * @brief TransportClock fed by a plugin host's playhead, with fallbacks.
 *
 * Each block the audio thread converts the host's position (JUCE
 * AudioPlayHead::PositionInfo, CLAP clap_event_transport, VST3 ProcessContext)
 * into a TransportInfo with validity flags and passes it to set_host_info()
 * before begin_block(). Fields the host omits come from the fallbacks set with
 * set_bpm(), set_time_signature(), play()/stop() and set_position_beats():
 *
 * - Tempo: host if k_has_tempo, else the fallback bpm. Latched per block. The
 *   block end is predicted with it; a tempo step or ramp inside the block (a DAW
 *   tempo map) shows up as a small start difference at the next block, which the
 *   ContinuityTracker absorbs (k_tempo_changed, not k_jumped).
 * - Play state: host k_is_playing if the host reported any musical field
 *   (tempo, beat, time signature, bar start or loop), else the fallback.
 * - Beat: host if k_has_beat_position (constant within the block while
 *   stopped), else free-running from the previous block's end at the current
 *   tempo (the fallback seek moves it). The free-run is computed from an
 *   integer sample count, so it does not drift.
 * - Time signature: host if k_has_time_signature, else the fallback.
 * - Bar start, loop and recording are passed through when present.
 *
 * Discontinuity flags (seek, loop wrap, start/stop, tempo change) come from a
 * ContinuityTracker, never from the host. A missing set_host_info() call means
 * "the host provided nothing this block".
 *
 * @par Real-Time Safety
 *   set_host_info(), begin_block(), end_block() and every getter are real-time
 *   safe. The fallback setters are lock-free atomics callable from any thread.
 */
class TANH_API HostTransportClock final : public TransportClock {
public:
    // ── Main thread ──────────────────────────────────────────────────────────
    void prepare(double sample_rate) override;

    // ── Audio thread only ─────────────────────────────────────────────────────

    /// The host's view of this block. Call before begin_block(); consumed by it.
    /// Discontinuity bits in @p info are ignored.
    void set_host_info(const TransportInfo& info) noexcept TANH_NONBLOCKING_FUNCTION;

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
    [[nodiscard]] uint32_t discontinuities() const override;

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

    // Free-run: beat = m_free_anchor_beat + m_free_samples * bpm / (60 * sr).
    double m_free_anchor_beat = 0.0;
    double m_free_bpm = 0.0;
    uint64_t m_free_samples = 0;
    bool m_free_valid = false;  // false → re-anchor at the previous block's end

    std::atomic<double> m_fallback_bpm{120.0};
    std::atomic<int> m_fallback_sig_num{4};
    std::atomic<int> m_fallback_sig_denom{4};
    std::atomic<bool> m_fallback_playing{false};
    std::atomic<double> m_pending_position_beats{0.0};
    std::atomic<bool> m_has_pending_position{false};
};

}  // namespace thl::dsp::transport
