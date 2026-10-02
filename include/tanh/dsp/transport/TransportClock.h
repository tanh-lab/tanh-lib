#pragma once

#include <tanh/core/Exports.h>
#include <tanh/dsp/transport/TransportInfo.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <cstdint>
#include <optional>

namespace thl::dsp::transport {

/**
 * @class TransportClock
 * @brief Abstract transport clock for sample-accurate timing.
 *
 * Driven entirely by the audio callback — no timers, no threads.
 * The sample counter is the authoritative time source; beat position
 * is derived from it on demand.
 *
 * @par Real-Time Safety
 *   begin_block() and end_block() must be real-time safe.
 *   beat_at_sample() must be real-time safe.
 *   Any-thread setters (set_bpm, play, stop, set_position_beats) must
 *   be lock-free.
 *
 * @par Audio-thread contract
 *   Call begin_block() once at the start of every process() call.
 *   Call end_block()   once at the end   of every process() call.
 *   beat_at_sample() and is_playing() are only valid between those two calls.
 *
 * @par Link compatibility
 *   host_time_micros in begin_block() carries the hardware output timestamp
 *   (callback host time + output latency). Ignored by InternalTransportClock,
 *   used by thl::link::LinkTransportClock.
 *
 * @par Block snapshot
 *   block_info() returns the whole block as a TransportInfo, including the
 *   discontinuity flags (seek, loop wrap, start/stop, tempo change, timeline
 *   reset). Consumers that take a `const TransportClock&` work unchanged with
 *   the host, internal and Link clocks.
 */
class TANH_API TransportClock {
public:
    virtual ~TransportClock() = default;

    virtual void prepare(double sample_rate) = 0;

    // ── Audio thread only ─────────────────────────────────────────────────────

    /**
     * @brief Latch pending state and cache derived values for this block.
     *
     * Must be the first call in every process() invocation.
     * BPM changes, play/stop, and seeks take effect here — never mid-block.
     *
     * @param frame_count       Number of samples in this block.
     * @param host_time_micros  Hardware output timestamp in microseconds.
     *                          Required for Link; pass nullopt otherwise.
     */
    virtual void begin_block(uint32_t frame_count,
                             std::optional<int64_t> host_time_micros = std::nullopt)
        TANH_NONBLOCKING_FUNCTION = 0;

    /**
     * @brief Finalise the block and advance the sample position.
     *        Must be the last call in every process() invocation.
     *
     * Advancement is deferred to end_block() rather than done at the top of
     * begin_block() so that beat_at_sample() computes offsets relative to the
     * start of the current block throughout processing.
     *
     * A future LinkTransportClock also requires this call to commit its audio
     * session state to the Link timeline at the correct moment.
     */
    virtual void end_block() TANH_NONBLOCKING_FUNCTION = 0;

    /**
     * @brief Beat position at a given sample offset within the current block.
     *
     * Returns a continuous musical time as a double — e.g. 1.75 means
     * three-quarters of the way through beat 2. Use this for modulation,
     * visualisation, or any calculation that needs raw musical time.
     *
     * This is distinct from Division, which has only five fixed granularities
     * and cannot represent an arbitrary position. From the returned value you
     * can derive anything:
     *
     *   beat_phase     = fmod(pos, 1.0)   // phase within the current beat
     *   bar_phase      = fmod(pos, 4.0)   // phase within the current bar
     *   sixteenth_idx  = pos * 4.0        // which 16th note we are on
     *
     * Returns the same value for all offsets when stopped.
     * Valid only between begin_block() and end_block().
     *
     * @param offset  Sample offset within [0, frame_count).
     */
    [[nodiscard]] virtual double beat_at_sample(uint32_t offset) const
        TANH_NONBLOCKING_FUNCTION = 0;

    /**
     * @brief Returns true if at least one boundary of @p div falls within the
     *        current block, using the half-open interval [block_start, block_end).
     *
     * A boundary that lands exactly on the first sample of the next block
     * belongs to the next block, not the current one.
     *
     * Uses the frame_count latched by begin_block() — no argument needed.
     * Valid only between begin_block() and end_block().
     */
    [[nodiscard]] virtual bool division_in_block(Division div) const TANH_NONBLOCKING_FUNCTION = 0;

    [[nodiscard]] virtual bool is_playing() const TANH_NONBLOCKING_FUNCTION = 0;
    [[nodiscard]] virtual double bpm() const TANH_NONBLOCKING_FUNCTION = 0;
    [[nodiscard]] virtual int sig_num() const TANH_NONBLOCKING_FUNCTION = 0;
    [[nodiscard]] virtual int sig_denom() const TANH_NONBLOCKING_FUNCTION = 0;
    [[nodiscard]] virtual uint64_t sample_position() const TANH_NONBLOCKING_FUNCTION = 0;

    // ── Any thread — lock-free ────────────────────────────────────────────────

    virtual void set_bpm(double bpm) TANH_NONBLOCKING_FUNCTION = 0;
    virtual void set_time_signature(int num, int denom) TANH_NONBLOCKING_FUNCTION = 0;
    virtual void play() TANH_NONBLOCKING_FUNCTION = 0;
    virtual void stop() TANH_NONBLOCKING_FUNCTION = 0;
    virtual void set_position_beats(double beats) TANH_NONBLOCKING_FUNCTION = 0;

    // ── Block snapshot (audio thread, between begin_block and end_block) ──────

    /**
     * @brief The current block as a TransportInfo.
     *
     * The default builds it from the getters (tempo, beat, time signature, play
     * state) and reports no discontinuities and no block length. The tanh clocks
     * override it with the tracked snapshot.
     */
    [[nodiscard]] virtual TransportInfo block_info() const TANH_NONBLOCKING_FUNCTION {
        TransportInfo info;
        info.m_flags = TransportInfo::k_has_tempo | TransportInfo::k_has_beat_position |
                       TransportInfo::k_has_time_signature;
        if (is_playing()) { info.m_flags |= TransportInfo::k_is_playing; }
        info.m_bpm = bpm();
        info.m_beat_position = beat_at_sample(0);
        info.m_beats_per_sample = beat_at_sample(1) - info.m_beat_position;
        info.m_sig_num = sig_num();
        info.m_sig_denom = sig_denom();
        info.m_quantum = beats_per_division(Division::Bar, info.m_sig_num, info.m_sig_denom);
        return info;
    }

    /// Discontinuity bits (TransportInfo::k_discontinuity_mask) of the current block.
    [[nodiscard]] virtual uint32_t discontinuities() const TANH_NONBLOCKING_FUNCTION { return 0; }
};

}  // namespace thl::dsp::transport
