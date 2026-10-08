#pragma once

#include <tanh/dsp/transport/ContinuityTracker.h>
#include <tanh/dsp/transport/HostTimeFilter.h>
#include <tanh/dsp/transport/LinkBackend.h>
#include <tanh/dsp/transport/TransportClock.h>
#include <tanh/dsp/transport/TransportInfo.h>

#include <atomic>
#include <cstdint>
#include <optional>

namespace thl::dsp::transport {

/**
 * @brief TransportClock that follows an Ableton Link session.
 *
 * Built over a LinkBackend, usually thl::link::LinkSession::timeline_backend()
 * (TANH_BUILD_LINK); the clock itself has no Link dependency. Pass the
 * callback's host time on the Link clock to set_host_time_us() before every
 * begin_block(), without output latency, and keep the latency current with
 * set_output_latency_samples(). Without a host time the sample counter is
 * mapped to Link time by a HostTimeFilter.
 *
 * - UI requests (tempo, play/stop, seek) are applied at the block's output time.
 *   A local start is quantized to the session's quantum (LinkBackend::quantum()).
 * - A start or stop dated ahead takes effect in the block that contains it;
 *   beats before a start are negative (count-in).
 * - The Link timeline keeps running while stopped (m_beats_per_sample > 0), so
 *   gate musical events on is_playing().
 * - A phase realignment is k_jumped. After Link is enabled or the first peer
 *   joins, the first realignment also gets k_timeline_reset.
 * - A peer's tempo change dated within 50 ms of the block start is
 *   k_tempo_changed, not k_jumped.
 * - Enabling Link or a peer joining never pushes the local tempo; set_bpm()
 *   requests are applied only when made.
 *
 * begin_block(), end_block() and the getters are real-time safe when the backend
 * is. The setters are lock-free and callable from any thread.
 */
class TANH_API LinkTransportClock final : public TransportClock {
public:
    explicit LinkTransportClock(LinkBackend& backend);

    // ── Any thread (atomics) ──────────────────────────────────────────────────
    /// Device output latency; re-send on device or audio-route changes.
    void set_output_latency_samples(uint32_t samples);
    /// The session's quantum in beats (the phase unit shared with peers).
    [[nodiscard]] double quantum() const { return m_backend.quantum(); }

    // ── Audio thread only ─────────────────────────────────────────────────────
    /// The callback's host time in microseconds on the Link clock, without output
    /// latency, for the next begin_block() (consumed by it).
    void set_host_time_us(int64_t host_time_us) TANH_NONBLOCKING_FUNCTION {
        m_host_time_us = host_time_us;
    }
    /// Output time in microseconds on the Link clock used for the current block.
    [[nodiscard]] int64_t output_time_us() const { return m_t0_us; }

    // ── Any thread — queued, applied in the next begin_block() ────────────────
    void set_bpm(double bpm) override;
    /// Local only, Link has no time signature.
    void set_time_signature(int num, int denom) override;
    void play() override;
    void stop() override;
    /// Requests @p beats at the output time; with peers only the phase is kept.
    void set_position_beats(double beats) override;

    /// Latency in µs for @p samples at @p sample_rate (rounded).
    [[nodiscard]] static int64_t latency_us(uint32_t samples, double sample_rate);

protected:
    void on_prepare(double sample_rate) override;
    [[nodiscard]] TransportInfo compute_block(uint32_t frame_count) override;

private:
    /// The block's output time: the host time (or the filter's) plus the latency.
    [[nodiscard]] int64_t output_time(uint32_t frame_count) TANH_NONBLOCKING_FUNCTION;
    /// Arms the timeline reset for the next realignment after an epoch change.
    void follow_epoch(int64_t t0) TANH_NONBLOCKING_FUNCTION;
    /// Applies the queued UI requests at @p t0 to the captured state (commits if
    /// any) and returns the state the block plays.
    [[nodiscard]] LinkState apply_requests(int64_t t0, double quantum) TANH_NONBLOCKING_FUNCTION;

    LinkBackend& m_backend;
    double m_sample_rate = 48000.0;
    double m_sample_time = 0.0;  // samples since prepare(), for the host-time filter
    std::optional<int64_t> m_host_time_us;
    int64_t m_t0_us = 0;
    uint64_t m_seen_epoch = 0;
    bool m_epoch_armed = false;
    int64_t m_epoch_armed_until_us = 0;
    double m_tempo_window_samples = 0.0;

    ContinuityTracker m_tracker;
    HostTimeFilter m_host_time_filter;

    std::atomic<uint32_t> m_latency_samples{0};
    std::atomic<double> m_pending_bpm{120.0};
    std::atomic<bool> m_has_pending_bpm{false};
    std::atomic<int> m_pending_play{0};  // 0 none, 1 play, -1 stop
    std::atomic<double> m_pending_position_beats{0.0};
    std::atomic<bool> m_has_pending_position{false};
    std::atomic<int> m_sig_num{4};
    std::atomic<int> m_sig_denom{4};
};

}  // namespace thl::dsp::transport
