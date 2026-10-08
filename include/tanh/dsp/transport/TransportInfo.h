#pragma once

#include <tanh/core/Exports.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <cmath>
#include <cstdint>
#include <type_traits>

namespace thl::dsp::transport {

/**
 * @brief Musical time divisions for division_in_block() and division_boundary_in().
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
 * @brief True if a boundary of @p div lies in the half-open beat interval [start, end).
 *
 * A boundary exactly at @p end belongs to the next block. Negative beats (a Link
 * count-in) are handled like positive ones. Returns false for an empty or reversed
 * interval.
 */
[[nodiscard]] inline bool division_boundary_in(double start,
                                               double end,
                                               Division div,
                                               int sig_num,
                                               int sig_denom) TANH_NONBLOCKING_FUNCTION {
    if (!(end > start)) { return false; }
    const double size = beats_per_division(div, sig_num, sig_denom);
    if (!(size > 0.0)) { return false; }
    const double next = std::ceil(start / size) * size;
    return next < end;
}

/**
 * @brief Per-block musical-time snapshot shared by every transport source.
 *
 * A TransportClock fills it in begin_block() from a host playhead, its internal
 * clock or Ableton Link, and consumers read it on the audio thread. It is
 * trivially copyable, so it can be published to a UI thread by value.
 *
 * Beats are quarter notes (JUCE ppq, VST3 projectTimeMusic). The beat at sample
 * `i` of the block is beat_at(i). Derive musical position from the absolute beat
 * every block and never accumulate beats; use the discontinuity flags only to
 * reset internal state. A beat below zero (Link count-in) means "not started yet".
 */
struct TransportInfo {
    /// Validity: the source provided this field.
    static constexpr uint32_t k_has_tempo = 1u << 0;
    static constexpr uint32_t k_has_beat_position = 1u << 1;
    static constexpr uint32_t k_has_time_signature = 1u << 2;

    /// Play state.
    static constexpr uint32_t k_is_playing = 1u << 8;

    /// The beat at the block start differs from the previous block's end beyond
    /// what jitter and a tempo change explain (seek, host loop wrap, Link phase
    /// realignment). Consumers react to this flag and never compare beats.
    static constexpr uint32_t k_jumped = 1u << 16;
    /// Stopped in the previous block, playing in this one.
    static constexpr uint32_t k_started = 1u << 17;
    /// Playing in the previous block, stopped in this one.
    static constexpr uint32_t k_stopped = 1u << 18;
    /// The tempo differs from the previous block's. The beat stays continuous.
    static constexpr uint32_t k_tempo_changed = 1u << 19;
    /// First block after prepare() or reset(), or a Link session join that moved
    /// the beat. Treat it like k_jumped and also clear edge detectors.
    static constexpr uint32_t k_timeline_reset = 1u << 20;

    static constexpr uint32_t k_discontinuity_mask =
        k_jumped | k_started | k_stopped | k_tempo_changed | k_timeline_reset;

    uint32_t m_flags = 0;
    uint32_t m_num_samples = 0;  ///< Frames of the block this snapshot describes.

    double m_bpm = 120.0;
    double m_beat_position = 0.0;     ///< Beat at sample 0 of the block (< 0: count-in).
    double m_beats_per_sample = 0.0;  ///< Slope inside the block (0 while held).
    double m_jump_delta_beats = 0.0;  ///< Beat(start) - expected beat, valid with k_jumped.
    int32_t m_sig_num = 4;
    int32_t m_sig_denom = 4;

    /// True if any bit of @p flag is set.
    [[nodiscard]] constexpr bool has(uint32_t flag) const TANH_NONBLOCKING_FUNCTION {
        return (m_flags & flag) != 0;
    }

    [[nodiscard]] constexpr bool is_playing() const TANH_NONBLOCKING_FUNCTION {
        return has(k_is_playing);
    }

    /// The discontinuity bits of this block (k_discontinuity_mask).
    [[nodiscard]] constexpr uint32_t discontinuities() const TANH_NONBLOCKING_FUNCTION {
        return m_flags & k_discontinuity_mask;
    }

    /// Beat at an in-block sample offset.
    [[nodiscard]] constexpr double beat_at(uint32_t offset) const TANH_NONBLOCKING_FUNCTION {
        return m_beat_position + static_cast<double>(offset) * m_beats_per_sample;
    }

    /// Beat at the end of the block, the next block's expected start.
    [[nodiscard]] constexpr double beat_end() const TANH_NONBLOCKING_FUNCTION {
        return beat_at(m_num_samples);
    }

    /**
     * @brief The part [offset, offset + length) of this block as a block of its own.
     *
     * For consumers that process a block in chunks. The discontinuity bits stay on
     * the chunk at offset 0, so the chunks read like consecutive continuous blocks.
     */
    [[nodiscard]] constexpr TransportInfo sub_block(uint32_t offset, uint32_t length) const
        TANH_NONBLOCKING_FUNCTION {
        TransportInfo t = *this;
        t.m_beat_position = beat_at(offset);
        t.m_num_samples = length;
        if (offset > 0) {
            t.m_flags &= ~k_discontinuity_mask;
            t.m_jump_delta_beats = 0.0;
        }
        return t;
    }

    /// Phase of beat_at(offset) in [0, unit); negative beats wrap like positive
    /// ones. Returns 0 for unit <= 0.
    [[nodiscard]] double phase(double unit, uint32_t offset = 0) const TANH_NONBLOCKING_FUNCTION {
        if (!(unit > 0.0)) { return 0.0; }
        double p = std::fmod(beat_at(offset), unit);
        if (p < 0.0) { p += unit; }
        if (p >= unit) { p = 0.0; }  // -tiny + unit rounds up to unit
        return p;
    }

    /// True if a boundary of @p div falls inside [beat_at(0), beat_end()).
    [[nodiscard]] bool division_in_block(Division div) const TANH_NONBLOCKING_FUNCTION {
        return division_boundary_in(m_beat_position, beat_end(), div, m_sig_num, m_sig_denom);
    }
};

static_assert(std::is_trivially_copyable_v<TransportInfo>);

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
