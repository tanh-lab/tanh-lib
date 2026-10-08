#pragma once

// Types shared by MotionRecorder and its parts (take recording, playback).

#include <tanh/modulation/MotionLane.h>

#include <cstddef>
#include <cstdint>

namespace thl::modulation {

/// Loop length of a take (arm, record) or of playback (set_playback_length).
/// Free lasts as long as the gesture; BarsN is N bars of the time signature,
/// synced to the transport.
enum class LoopLength : uint8_t { Free, Bars1, Bars2, Bars4, Bars8, Bars16 };

/// Bars of @p length; 0 for LoopLength::Free.
[[nodiscard]] constexpr uint32_t loop_length_bars(LoopLength length) {
    switch (length) {
        case LoopLength::Free: return 0;
        case LoopLength::Bars1: return 1;
        case LoopLength::Bars2: return 2;
        case LoopLength::Bars4: return 4;
        case LoopLength::Bars8: return 8;
        case LoopLength::Bars16: return 16;
    }
    return 0;
}

struct MotionRecorderConfig {
    /// Points per take buffer (two buffers per recorder, 11 bytes per point).
    size_t m_max_points = k_motion_default_max_points;
    /// Grid rate of Seconds takes in points per second. Beats takes use
    /// round(rate * 60 / bpm) ticks per beat, clamped to [m_min_tpb, m_max_tpb].
    double m_rate = 200.0;
    uint32_t m_min_tpb = 24;  ///< below this a bar take is refused (capacity)
    uint32_t m_max_tpb = 512;
    /// Catmull-Rom is evaluated every this many samples; the output ramps linearly between.
    uint32_t m_render_interval = 32;
    double m_glide_ms = 25.0;  ///< raised-cosine glide after a jump, a new lane or a release
    /// Every take becomes a Beats lane of whole bars:
    /// - BarsN starts on the bar line at or before the first touch, records
    ///   lifts as gate 0 and lasts exactly N bars; disarm() and stop() pad it
    ///   with gate 0 instead of dropping it.
    /// - Free runs from the first touch to the release (16 bars at most), drops
    ///   the still ends and is stretched onto the nearest of 1, 2, 4, 8 or 16
    ///   bars, so it loops without a pause. Lifting ends it: use BarsN to
    ///   record gaps.
    /// Off: free takes end on release and keep their length.
    bool m_bar_aligned_takes = false;
};

}  // namespace thl::modulation
