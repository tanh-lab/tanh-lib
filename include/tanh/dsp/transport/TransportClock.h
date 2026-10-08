#pragma once

#include <tanh/core/Exports.h>
#include <tanh/dsp/transport/TransportInfo.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <cstdint>

namespace thl::dsp::transport {

/**
 * @class TransportClock
 * @brief Abstract transport clock for sample-accurate timing.
 *
 * Driven entirely by the audio callback — no timers, no threads. Every block,
 * begin_block() asks the clock for the block's TransportInfo (compute_block());
 * the getters read that snapshot, so they mean the same on every clock.
 *
 * @par Real-Time Safety
 *   begin_block(), end_block() and the getters are real-time safe.
 *   Any-thread setters (set_bpm, play, stop, set_position_beats) are lock-free.
 *
 * @par Audio-thread contract
 *   Call begin_block() once at the start of every process() call.
 *   Call end_block()   once at the end   of every process() call.
 *   The getters are only valid between those two calls.
 *
 * Clock-specific inputs are set before begin_block():
 * HostTransportClock::set_host_info() and LinkTransportClock::set_host_time_us().
 */
class TANH_API TransportClock {
public:
    TransportClock() = default;
    virtual ~TransportClock() = default;

    TransportClock(const TransportClock&) = delete;
    TransportClock& operator=(const TransportClock&) = delete;
    TransportClock(TransportClock&&) = delete;
    TransportClock& operator=(TransportClock&&) = delete;

    /// Message thread, audio stopped: sets the sample rate and starts over (the
    /// next block is a k_timeline_reset, sample_position() is 0).
    void prepare(double sample_rate) {
        m_info = TransportInfo{};
        m_sample_position = 0;
        on_prepare(sample_rate);
    }

    // ── Audio thread only ─────────────────────────────────────────────────────

    /**
     * @brief Latch pending state and compute this block.
     *
     * Must be the first call in every process() invocation. BPM changes,
     * play/stop and seeks take effect here — never mid-block.
     */
    void begin_block(uint32_t frame_count) TANH_NONBLOCKING_FUNCTION {
        m_info = compute_block(frame_count);
    }

    /// Finalise the block. Must be the last call in every process() invocation.
    void end_block() TANH_NONBLOCKING_FUNCTION {
        if (m_info.is_playing()) { m_sample_position += m_info.m_num_samples; }
    }

    /// The current block, including the discontinuity flags.
    [[nodiscard]] const TransportInfo& block_info() const TANH_NONBLOCKING_FUNCTION {
        return m_info;
    }

    /**
     * @brief Beat position at a given sample offset within the current block.
     *
     * Continuous musical time in quarter notes, e.g. 1.75 is three quarters
     * through beat 2. Derive phases from it (fmod(pos, 1.0) within the beat) or
     * use TransportInfo::phase().
     *
     * @param offset  Sample offset within [0, frame_count).
     */
    [[nodiscard]] double beat_at_sample(uint32_t offset) const TANH_NONBLOCKING_FUNCTION {
        return m_info.beat_at(offset);
    }

    /**
     * @brief True while playing if a boundary of @p div falls within the current
     *        block, using the half-open interval [block_start, block_end).
     *
     * A boundary exactly on the first sample of the next block belongs to the next
     * block.
     */
    [[nodiscard]] bool division_in_block(Division div) const TANH_NONBLOCKING_FUNCTION {
        return m_info.is_playing() && m_info.division_in_block(div);
    }

    [[nodiscard]] bool is_playing() const TANH_NONBLOCKING_FUNCTION { return m_info.is_playing(); }
    [[nodiscard]] double bpm() const TANH_NONBLOCKING_FUNCTION { return m_info.m_bpm; }
    [[nodiscard]] int sig_num() const TANH_NONBLOCKING_FUNCTION { return m_info.m_sig_num; }
    [[nodiscard]] int sig_denom() const TANH_NONBLOCKING_FUNCTION { return m_info.m_sig_denom; }
    /// Samples processed while playing since prepare().
    [[nodiscard]] uint64_t sample_position() const TANH_NONBLOCKING_FUNCTION {
        return m_sample_position;
    }

    // ── Any thread — lock-free ────────────────────────────────────────────────

    virtual void set_bpm(double bpm) TANH_NONBLOCKING_FUNCTION = 0;
    virtual void set_time_signature(int num, int denom) TANH_NONBLOCKING_FUNCTION = 0;
    virtual void play() TANH_NONBLOCKING_FUNCTION = 0;
    virtual void stop() TANH_NONBLOCKING_FUNCTION = 0;
    virtual void set_position_beats(double beats) TANH_NONBLOCKING_FUNCTION = 0;

protected:
    /// prepare() for the subclass: the sample rate, and starting over.
    virtual void on_prepare(double sample_rate) = 0;

    /**
     * @brief The block of @p frame_count samples starting now.
     *
     * Called by begin_block(). block_info() still holds the previous block (the
     * default TransportInfo after prepare()). Typically fills the raw beats and
     * resolves them with a ContinuityTracker.
     */
    [[nodiscard]] virtual TransportInfo compute_block(uint32_t frame_count)
        TANH_NONBLOCKING_FUNCTION = 0;

private:
    TransportInfo m_info{};
    uint64_t m_sample_position = 0;
};

}  // namespace thl::dsp::transport
