#pragma once

#include <tanh/dsp/transport/LinkBackend.h>
#include <tanh/dsp/transport/TransportClock.h>
#include <tanh/dsp/transport/TransportInfo.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <optional>

namespace thl::dsp::transport {

/**
 * @class LinkTransportClock
 * @brief TransportClock that follows an Ableton Link session.
 *
 * Construct it over a LinkBackend: `LinkTransportClock clock(session.audio_backend());`
 * with a thl::link::LinkSession (TANH_WITH_LINK), or over a fake in tests. The
 * clock itself has no Link dependency.
 *
 * begin_block() (audio thread):
 * 1. Output time t0 = callback host time + output latency. Pass the callback's
 *    host time in microseconds on the Link clock (JUCE `hostTimeNs / 1000`,
 *    CoreAudio `mHostTime` converted to µs) *without* latency, and keep the
 *    latency current with set_output_latency_samples(). Without a host time a
 *    linear-regression filter maps the sample counter to Link time.
 * 2. capture(), apply queued UI requests at t0 (tempo, play/stop, seek), on a
 *    local start request requestBeatAtStartPlayingTime(0, quantum) (quantized
 *    launch), commit() only if something changed.
 * 3. Beats at t0 and at the block's end feed a ContinuityTracker, which absorbs
 *    host-time jitter and flags a phase realignment as k_jumped. When Link was
 *    enabled or the first peer joined (LinkBackend::timeline_epoch()) and the beat
 *    moved, k_timeline_reset is reported too. A peer's tempo change dated
 *    within ±50 ms of the block start (peers date it at their output time) is
 *    a tempo change, not a jump (ContinuityTracker::set_tempo_window_samples()),
 *    except in a block where the epoch changed.
 *
 * Play state: the block plays if the session plays and its start time lies
 * before the block's end, so k_started lands in the block that contains the
 * start; beats before it are negative (count-in). Likewise a stop dated ahead
 * (a peer stopping at its output time) lands as k_stopped in the block that
 * contains it. Unlike the host and internal clocks the Link timeline keeps
 * running while stopped (m_beats_per_sample > 0); gate musical events on
 * is_playing().
 *
 * Tempo requests (set_bpm) are applied only when made; enabling Link or a peer
 * joining never pushes the local tempo (Link TEST-PLAN TEMPO-5). Do not push a
 * preset tempo while peers are connected (TEMPO-2/3).
 *
 * @par Real-Time Safety
 *   begin_block(), end_block() and the getters are real-time safe when the
 *   backend is. Setters are lock-free atomics callable from any thread.
 */
class TANH_API LinkTransportClock final : public TransportClock {
public:
    explicit LinkTransportClock(LinkBackend& backend);

    // ── Main thread ──────────────────────────────────────────────────────────
    void prepare(double sample_rate) override;

    // ── Any thread (atomics) ──────────────────────────────────────────────────
    /// Device output latency; re-send on device or audio-route changes.
    void set_output_latency_samples(uint32_t samples) noexcept;
    /// Link quantum in beats (phase unit shared with peers), default 4.
    void set_quantum(double beats) noexcept;
    [[nodiscard]] double quantum() const noexcept;

    // ── Audio thread only ─────────────────────────────────────────────────────
    void begin_block(uint32_t frame_count,
                     std::optional<int64_t> host_time_micros = std::nullopt) override;
    void end_block() override;

    [[nodiscard]] double beat_at_sample(uint32_t offset) const override;
    [[nodiscard]] bool division_in_block(Division div) const override;
    [[nodiscard]] bool is_playing() const override;
    [[nodiscard]] double bpm() const override;
    [[nodiscard]] int sig_num() const override;
    [[nodiscard]] int sig_denom() const override;
    [[nodiscard]] uint64_t sample_position() const override;
    [[nodiscard]] TransportInfo block_info() const override;
    [[nodiscard]] uint32_t discontinuities() const override;
    /// Output time (t0, µs) used for the current block.
    [[nodiscard]] int64_t output_time_us() const noexcept { return m_t0_us; }

    // ── Any thread — queued, applied in the next begin_block() ────────────────
    void set_bpm(double bpm) override;
    /// Local only: Link has no time signature.
    void set_time_signature(int num, int denom) override;
    void play() override;
    void stop() override;
    /// requestBeatAtTime(beats, t0, quantum): with peers only the phase is kept.
    void set_position_beats(double beats) override;

    /// Latency in µs for @p samples at @p sample_rate (rounded).
    [[nodiscard]] static int64_t latency_us(uint32_t samples, double sample_rate) noexcept;

private:
    // Linear regression sample time → host time over the last k_filter_points
    // blocks (Ableton's HostTimeFilter, fixed storage).
    static constexpr size_t k_filter_points = 512;
    int64_t filtered_host_time_us(double sample_time) noexcept TANH_NONBLOCKING_FUNCTION;

    LinkBackend& m_backend;
    double m_sample_rate = 48000.0;
    uint64_t m_sample_position = 0;
    int64_t m_t0_us = 0;
    uint64_t m_seen_epoch = 0;
    double m_tempo_window_samples = 0.0;

    TransportInfo m_info{};
    ContinuityTracker m_tracker;

    std::array<double, k_filter_points> m_filter_x{};
    std::array<double, k_filter_points> m_filter_y{};
    size_t m_filter_count = 0;
    size_t m_filter_index = 0;
    double m_filter_x0 = 0.0;
    double m_filter_y0 = 0.0;

    std::atomic<uint32_t> m_latency_samples{0};
    std::atomic<double> m_quantum{4.0};
    std::atomic<double> m_pending_bpm{120.0};
    std::atomic<bool> m_has_pending_bpm{false};
    std::atomic<int> m_pending_play{0};  // 0 none, 1 play, -1 stop
    std::atomic<double> m_pending_position_beats{0.0};
    std::atomic<bool> m_has_pending_position{false};
    std::atomic<int> m_sig_num{4};
    std::atomic<int> m_sig_denom{4};
};

}  // namespace thl::dsp::transport
