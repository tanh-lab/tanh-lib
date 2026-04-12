#pragma once

#include <cstdint>
#include <optional>

#include <tanh/core/Exports.h>

namespace thl {

/**
 * @brief Musical time divisions for division_in_block().
 *
 * All divisions are expressed as beat multiples relative to a quarter note.
 * Bar depends on the active time signature numerator (e.g. 4 beats in 4/4).
 */
enum class Division {
    Bar,        ///< One full bar  (sig_num quarter notes)
    Half,       ///< Half note     (2 quarter notes)
    Beat,       ///< Quarter note  (1 beat)
    Eighth,     ///< Eighth note   (0.5 beats)
    Sixteenth,  ///< Sixteenth note (0.25 beats)
};

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
                             std::optional<int64_t> host_time_micros = std::nullopt) = 0;

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
    virtual void end_block() = 0;

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
    [[nodiscard]] virtual double beat_at_sample(uint32_t offset) const = 0;

    /**
     * @brief Returns true if at least one boundary of @p div falls within the
     *        current block.
     *
     * Uses the frame_count latched by begin_block() — no argument needed.
     * O(1): two floor() calls, no per-sample iteration.
     * Valid only between begin_block() and end_block().
     */
    [[nodiscard]] virtual bool division_in_block(Division div) const = 0;

    [[nodiscard]] virtual bool is_playing() const = 0;
    [[nodiscard]] virtual double bpm() const = 0;
    [[nodiscard]] virtual int sig_num() const = 0;
    [[nodiscard]] virtual uint64_t sample_position() const = 0;

    // ── Any thread — lock-free ────────────────────────────────────────────────

    virtual void set_bpm(double bpm) = 0;
    virtual void set_time_signature(int num, int denom) = 0;
    virtual void play() = 0;
    virtual void stop() = 0;
    virtual void set_position_beats(double beats) = 0;
};

}  // namespace thl
