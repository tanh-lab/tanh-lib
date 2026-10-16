#pragma once

#include <tanh/core/Exports.h>
#include <tanh/dsp/transport/TransportInfo.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <cstdint>

namespace thl::dsp::transport {

/**
 * @brief Turns a clock's raw per-block beats into continuous, flagged blocks.
 *
 * Shared by every clock. The clock fills a TransportInfo with the raw start beat,
 * the tempo and the validity and play flags, then calls resolve() with the raw end
 * beat. A start close to the previous block's end is snapped onto it (host ppq
 * jitter, Link host-time jitter, in-block tempo changes); a start further away is
 * reported as k_jumped.
 */
class TANH_API ContinuityTracker {
public:
    /// Sets the sample rate (used for the tolerances) and calls reset().
    void prepare(double sample_rate);

    /// The next resolved block gets k_timeline_reset and no other discontinuity.
    void reset() TANH_NONBLOCKING_FUNCTION;

    /**
     * @brief Resolves one block.
     *
     * Reads the raw m_beat_position, m_bpm and m_flags of @p io, and writes the
     * resolved m_beat_position, m_beats_per_sample, m_num_samples (= @p frames),
     * m_jump_delta_beats and the discontinuity bits. @p raw_end_beat is the raw beat
     * at sample @p frames (equal to the start while the source holds).
     */
    void resolve(TransportInfo& io, double raw_end_beat, uint32_t frames) TANH_NONBLOCKING_FUNCTION;

    /// Jitter tolerance in samples (default 2). Kept across prepare() and reset().
    void set_tolerance_samples(double samples);

    /**
     * @brief How far from the block start a tempo change may be dated (default 0).
     *
     * For a source whose tempo changes are dated away from the block that first
     * sees them (a Link peer applies a change at its own output time). Hosts keep 0.
     * Kept across prepare() and reset().
     */
    void set_tempo_window_samples(double samples);

    /// Jitter tolerance in beats at @p bpm.
    [[nodiscard]] double tolerance_beats(double bpm) const;

private:
    double m_sample_rate = 48000.0;
    double m_tolerance_samples = 2.0;
    double m_tempo_window_samples = 0.0;
    double m_prev_end = 0.0;
    double m_prev_bpm = 0.0;
    double m_prev_frames = 0.0;
    bool m_prev_moving = false;
    bool m_prev_playing = false;
    bool m_reset_pending = true;
};

}  // namespace thl::dsp::transport
