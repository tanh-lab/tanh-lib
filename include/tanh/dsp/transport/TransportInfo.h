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
                                               int sig_denom) noexcept TANH_NONBLOCKING_FUNCTION {
    if (!(end > start)) { return false; }
    const double size = beats_per_division(div, sig_num, sig_denom);
    if (!(size > 0.0)) { return false; }
    const double next = std::ceil(start / size) * size;
    return next < end;
}

/**
 * @brief Per-block musical-time snapshot shared by every transport source.
 *
 * Filled by a TransportClock in begin_block() (host playhead, internal clock or
 * Ableton Link) and read by consumers on the audio thread. Trivially copyable, so
 * it can be published to a UI thread through a seqlock or triple buffer.
 *
 * Units: beats are quarter notes (JUCE ppq, VST3 projectTimeMusic). The beat at
 * sample `i` of the block is beat_at(i) = m_beat_position + i * m_beats_per_sample.
 *
 * @par Flags
 * Bits 0..15 say what the source provided (validity) and its play state. Bits
 * 16..20 are discontinuities against the previous block; only a clock's
 * ContinuityTracker sets them, never a host adapter.
 *
 * @par Consumer contract
 * Derive musical position statelessly from the absolute beat each block
 * (`phase(loop_len)`, `floor(beat / step_len)`); never accumulate beats. Use the
 * discontinuity flags only to reset internal state (edge detectors, smoothers,
 * takes). A beat below zero (Link count-in) means "not started yet".
 */
struct TransportInfo {
    // ── Validity: what the source provided ─────────────────────────────────
    static constexpr uint32_t k_has_tempo = 1u << 0;
    static constexpr uint32_t k_has_beat_position = 1u << 1;
    static constexpr uint32_t k_has_time_signature = 1u << 2;
    static constexpr uint32_t k_has_bar_start = 1u << 3;
    static constexpr uint32_t k_has_loop = 1u << 4;
    static constexpr uint32_t k_has_host_time = 1u << 5;

    // ── State ──────────────────────────────────────────────────────────────
    static constexpr uint32_t k_is_playing = 1u << 8;
    static constexpr uint32_t k_is_looping = 1u << 9;
    static constexpr uint32_t k_is_recording = 1u << 10;

    // ── Discontinuities vs. the previous block (set by the clock only) ─────
    /// beat(start) differs from the previous block's beat(end) beyond the
    /// tolerance: seek, host loop wrap, Link phase realignment.
    /// m_jump_delta_beats holds beat(start) - expected.
    static constexpr uint32_t k_jumped = 1u << 16;
    static constexpr uint32_t k_started = 1u << 17;  ///< stopped → playing
    static constexpr uint32_t k_stopped = 1u << 18;  ///< playing → stopped
    /// |bpm - previous bpm| > 1e-6. Not a jump: the beat stays continuous.
    static constexpr uint32_t k_tempo_changed = 1u << 19;
    /// First block after prepare()/reset(), a clock switch or Link enable. No
    /// k_jumped/k_started is reported for this block; treat it like k_jumped and
    /// also clear edge detectors.
    static constexpr uint32_t k_timeline_reset = 1u << 20;

    static constexpr uint32_t k_discontinuity_mask =
        k_jumped | k_started | k_stopped | k_tempo_changed | k_timeline_reset;

    uint32_t m_flags = 0;
    uint32_t m_num_samples = 0;  ///< frames of the block this snapshot describes

    double m_bpm = 120.0;
    double m_beat_position = 0.0;     ///< beat at sample 0 of the block (< 0: count-in)
    double m_beats_per_sample = 0.0;  ///< slope inside the block (0 while held)
    double m_bar_start_beats = 0.0;   ///< beat of the last bar start (k_has_bar_start)
    double m_loop_start_beats = 0.0;  ///< host loop (k_has_loop)
    double m_loop_end_beats = 0.0;
    double m_jump_delta_beats = 0.0;  ///< beat(start) - expected, valid with k_jumped
    double m_quantum = 4.0;           ///< phase unit: Link quantum, else beats per bar
    int64_t m_host_time_ns = 0;       ///< callback host time (k_has_host_time)
    int32_t m_sig_num = 4;
    int32_t m_sig_denom = 4;

    [[nodiscard]] constexpr bool has(uint32_t flag) const noexcept TANH_NONBLOCKING_FUNCTION {
        return (m_flags & flag) != 0;
    }

    [[nodiscard]] constexpr bool is_playing() const noexcept TANH_NONBLOCKING_FUNCTION {
        return has(k_is_playing);
    }

    /// Discontinuity bits of this block (k_discontinuity_mask).
    [[nodiscard]] constexpr uint32_t discontinuities() const noexcept TANH_NONBLOCKING_FUNCTION {
        return m_flags & k_discontinuity_mask;
    }

    /// Beat at an in-block sample offset.
    [[nodiscard]] constexpr double beat_at(uint32_t offset) const noexcept
        TANH_NONBLOCKING_FUNCTION {
        return m_beat_position + static_cast<double>(offset) * m_beats_per_sample;
    }

    /// Beat at the end of the block (= beat_at(m_num_samples), the next block's expected start).
    [[nodiscard]] constexpr double beat_end() const noexcept TANH_NONBLOCKING_FUNCTION {
        return beat_at(m_num_samples);
    }

    /// Phase of beat_at(offset) in [0, unit). Negative beats wrap like positive ones
    /// (-0.5 with unit 4 → 3.5). Returns 0 for unit <= 0.
    [[nodiscard]] double phase(double unit,
                               uint32_t offset = 0) const noexcept TANH_NONBLOCKING_FUNCTION {
        if (!(unit > 0.0)) { return 0.0; }
        double p = std::fmod(beat_at(offset), unit);
        if (p < 0.0) { p += unit; }
        if (p >= unit) { p = 0.0; }  // -tiny + unit rounds up to unit
        return p;
    }

    /// True if a boundary of @p div falls inside [beat_at(0), beat_end()).
    [[nodiscard]] bool division_in_block(Division div) const noexcept TANH_NONBLOCKING_FUNCTION {
        return division_boundary_in(m_beat_position, beat_end(), div, m_sig_num, m_sig_denom);
    }
};

static_assert(std::is_trivially_copyable_v<TransportInfo>);

/**
 * @brief Turns raw per-block (beat, bpm, playing) into continuous, flagged output.
 *
 * Shared by every clock. The clock fills a TransportInfo with the raw beat at the
 * block start, the bpm and the validity/play flags, then calls resolve() with the
 * raw beat at the block end. resolve() compares the start against the previous
 * block's end:
 *
 * - |start - expected| <= tolerance: the start snaps to the expected beat and the
 *   slope is recomputed to land on the raw end beat. This absorbs host ppq jitter
 *   and Link host-time jitter without accumulating error.
 * - beyond the tolerance: k_jumped with m_jump_delta_beats = start - expected.
 *
 * tolerance = max(N samples worth of beats at the current bpm, 1e-6), N = 2 by
 * default (set_tolerance_samples()).
 * It also sets k_started / k_stopped / k_tempo_changed, and k_timeline_reset on the
 * first block after prepare() or reset().
 *
 * @par Real-Time Safety
 *   resolve() is real-time safe. prepare() and reset() are plain stores; call them
 *   from the audio thread or while it is stopped.
 */
class TANH_API ContinuityTracker {
public:
    /// Set the sample rate (used for the tolerance) and reset().
    void prepare(double sample_rate) noexcept;

    /// The next resolved block gets k_timeline_reset and no other discontinuity.
    void reset() noexcept TANH_NONBLOCKING_FUNCTION;

    /**
     * @brief Resolve one block.
     *
     * In: io.m_beat_position (raw start), io.m_bpm, io.m_flags (validity + play
     * state). Out: m_beat_position, m_beats_per_sample, m_num_samples = frames,
     * m_jump_delta_beats and the discontinuity bits (previous ones are cleared).
     *
     * @param raw_end_beat  raw beat at sample `frames` (== start when the source holds).
     */
    void resolve(TransportInfo& io,
                 double raw_end_beat,
                 uint32_t frames) noexcept TANH_NONBLOCKING_FUNCTION;

    /// Jitter tolerance in samples (default 2). Not reset by prepare()/reset().
    void set_tolerance_samples(double samples) noexcept;

    /// Tolerance used for the given bpm.
    [[nodiscard]] double tolerance_beats(double bpm) const noexcept TANH_NONBLOCKING_FUNCTION;

private:
    double m_sample_rate = 48000.0;
    double m_tolerance_samples = 2.0;
    double m_prev_end = 0.0;
    double m_prev_bpm = 0.0;
    bool m_prev_playing = false;
    bool m_reset_pending = true;
};

}  // namespace thl::dsp::transport
