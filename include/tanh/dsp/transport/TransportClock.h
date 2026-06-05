#pragma once

#include <cstdint>
#include <optional>

#include <tanh/core/Exports.h>
#include <tanh/utils/RealtimeSanitizer.h>

namespace thl::dsp::transport {

/**
 * @brief Musical time divisions for division_in_block().
 *
 * All divisions are expressed as beat multiples where 1 beat = quarter note.
 * Bar length depends on the active time signature: sig_num * (4 / sig_denom).
 * For example 4/4 → 4 beats, 6/8 → 3 beats, 7/8 → 3.5 beats.
 */
enum class Division {
    Bar,
    Half,       ///< Half note      (2 beats)
    Beat,       ///< Quarter note   (1 beat)
    Eighth,     ///< Eighth note    (0.5 beats)
    Sixteenth,  ///< Sixteenth note (0.25 beats)
    NumDivisions
};

/**
 * @brief Beat length of a division. Bar = sig_num * (4 / sig_denom).
 */
[[nodiscard]] inline double beats_per_division(Division div, int sig_num, int sig_denom) {
    switch (div) {
        case Division::Bar:
            return static_cast<double>(sig_num) * 4.0 / static_cast<double>(sig_denom);
        case Division::Half: return 2.0;
        case Division::Beat: return 1.0;
        case Division::Eighth: return 0.5;
        case Division::Sixteenth: return 0.25;
        case Division::NumDivisions: break;
    }
    return 1.0;
}

/**
 * @brief Map an int to a Division, clamped to a valid value.
 *        Returns Division::Beat for out-of-range input.
 */
[[nodiscard]] inline Division division_from_int(int value) {
    if (value < 0 || value >= static_cast<int>(Division::NumDivisions)) { return Division::Beat; }
    return static_cast<Division>(value);
}

/**
 * @class TransportClock
 * @brief Abstract transport clock for sample-accurate timing.
 *
 * Driven entirely by the audio callback — no timers, no threads.
 * The sample counter is the authoritative time source; beat position
 * is derived from it on demand.
 *
 * @section rt_safety Real-Time Safety
 *   begin_block() and end_block() must be real-time safe.
 *   beat_at_sample() must be real-time safe.
 *   Any-thread setters (set_bpm, play, stop, set_position_beats) must
 *   be lock-free.
 *
 * @section usage Audio-thread contract
 *   Call begin_block() once at the start of every process() call.
 *   Call end_block()   once at the end   of every process() call.
 *   beat_at_sample() and is_playing() are only valid between those two calls.
 *
 * @section link Link compatibility
 *   host_time_micros in begin_block() carries the hardware output timestamp
 *   (callback host time + output latency). Ignored by InternalTransportClock,
 *   required by a future LinkTransportClock.
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
};

}  // namespace thl::dsp::transport
