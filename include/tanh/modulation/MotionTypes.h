#pragma once

// Types shared by MotionRecorder and its parts (take recording, playback).

#include <tanh/modulation/MotionLane.h>

#include <cstddef>
#include <cstdint>
#include <span>

namespace thl::modulation {

/// Loop length of a take (arm, record) or of playback (set_playback_length).
/// free() lasts as long as the gesture; bars(n, division) is n / division bars of the time
/// signature, synced to the transport: bars(1, 4) a quarter bar, bars(3) three bars, bars(3, 2)
/// a bar and a half. Any length fits that MotionRecorderConfig::m_max_points holds at
/// m_min_tpb; a longer take is refused, so a recorder for long loops is given more points.
struct LoopLength {
    uint16_t m_bars = 0;      ///< 0: free
    uint16_t m_division = 1;  ///< 0: free

    [[nodiscard]] static constexpr LoopLength free() { return {}; }
    [[nodiscard]] static constexpr LoopLength bars(uint16_t bars, uint16_t division = 1) {
        return {.m_bars = bars, .m_division = division};
    }

    [[nodiscard]] constexpr bool is_free() const { return m_bars == 0 || m_division == 0; }

    /// The same length: bars(2, 4) is bars(1, 2), and every free length is free().
    friend constexpr bool operator==(LoopLength a, LoopLength b) {
        if (a.is_free() || b.is_free()) { return a.is_free() == b.is_free(); }
        return static_cast<uint32_t>(a.m_bars) * b.m_division ==
               static_cast<uint32_t>(b.m_bars) * a.m_division;
    }
    friend constexpr bool operator!=(LoopLength a, LoopLength b) { return !(a == b); }
};

/// Bars of @p length; 0 for LoopLength::free().
[[nodiscard]] constexpr double loop_length_bars(LoopLength length) {
    if (length.is_free()) { return 0.0; }
    return static_cast<double>(length.m_bars) / static_cast<double>(length.m_division);
}

struct MotionRecorderConfig {
    /// Points of a take (the take and two hand-over slots per recorder, 11 bytes per point).
    size_t m_max_points = k_motion_default_max_points;
    /// Recording rate in points per second: round(rate * 60 / bpm) points per beat, clamped to
    /// [m_min_tpb, m_max_tpb].
    double m_rate = 200.0;
    uint32_t m_min_tpb = 24;  ///< below this a bar take is refused (capacity)
    uint32_t m_max_tpb = 512;
    /// Catmull-Rom is evaluated every this many samples; the output ramps linearly between.
    uint32_t m_render_interval = 32;
    double m_glide_ms = 25.0;  ///< raised-cosine glide after a jump, a new lane or a release
};

/// Recorder state as a UI shows it.
enum class MotionState : uint8_t {
    Idle,       ///< nothing playing (no lane, or playback stopped) and not armed
    Armed,      ///< waiting for the first touch; the old lane keeps playing
    Recording,  ///< a take is being written
    Playing     ///< a lane is playing
};

/// One block of touch input for MotionRecorder::process().
struct MotionInput {
    const float* m_x = nullptr;  ///< [0, 1]
    const float* m_y = nullptr;  ///< [0, 1]
    const uint8_t* m_active = nullptr;
    std::span<const uint32_t> m_change_points;  ///< sorted offsets where an input event landed
    uint32_t m_num_samples = 0;
};

/// Recorder state at the end of the last process() call.
struct MotionSnapshot {
    MotionState m_state = MotionState::Idle;
    bool m_playing = false;  ///< playback enabled (play() / stop())
    bool m_reverse = false;
    bool m_beats = false;     ///< the playing lane is a Beats lane
    bool m_busy = false;      ///< armed, but both take buffers wait for service()
    bool m_refused = false;   ///< the last take could not start (tpb below the minimum)
    bool m_aborted = false;   ///< the last take was aborted (prepare() during a take)
    float m_phase = 0.0f;     ///< playback position, 0..1 of the loop
    float m_progress = 0.0f;  ///< recording progress, 0..1 of the current loop target
    uint32_t m_take_id = 0;   ///< id of the lane that is playing (0 = none)
    double m_length = 0.0;    ///< loop length of that lane (seconds or beats)
};

}  // namespace thl::modulation
