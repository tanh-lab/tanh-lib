#pragma once

#include <tanh/core/Exports.h>
#include <tanh/modulation/MotionLane.h>
#include <tanh/modulation/MotionTypes.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace thl::modulation::detail {

/// A loop of points as it plays: what a take holds, or what a take starts from.
struct MotionLoopView {
    const float* m_x = nullptr;
    const float* m_y = nullptr;
    const uint8_t* m_gate = nullptr;  ///< null: every point down
    size_t m_num_points = 0;
    MotionTimebase m_timebase = MotionTimebase::Beats;
    double m_length = 0.0;  ///< seconds or beats
    double m_anchor = 0.0;  ///< Beats: beat of point 0 (mod m_length)
    size_t m_seam = 0;      ///< index of the oldest point: the step closes before it
    [[nodiscard]] bool empty() const { return m_num_points == 0 || !(m_length > 0.0); }
};

/// Where and how a take starts.
struct MotionTakeStart {
    LoopLength m_length = LoopLength::Free;
    double m_bpm = 120.0;
    int m_sig_num = 4;
    int m_sig_denom = 4;
    double m_beat = 0.0;  ///< the recording clock's beat at the start sample
    float m_x = 0.5f;     ///< where the output is: a lifted point holds it until the touch
    float m_y = 0.5f;
    /// BarsN: what plays; the loop starts as it (a Beats loop; anything else is ignored).
    MotionLoopView m_seed;
};

/**
 * @brief Records takes into one preallocated working buffer (audio thread).
 *
 * Every take is a Beats loop of whole bars on a grid of points per bar.
 *
 * - Free: points from the first touch to the release, at most 16 bars. The still ends are
 *   dropped (one point kept), and the take is stretched onto the nearest (by ratio) of 1, 2,
 *   4, 8 or 16 bars, point 0 on the bar line nearest the first kept point. Fewer than
 *   k_motion_min_points points: Dropped.
 * - BarsN: a loop of N bars from the loop line (the multiple of N bars at or before the start).
 *   It starts as the seed, or lifted (gate 0) at the start position without one, and records
 *   until end(): where the finger is down it overwrites, where it is up the loop keeps what it
 *   holds (a lifted point it never held keeps the last position, gate 0). Every release
 *   completes the loop (the rest of a first pass is lifted) and is a Revision. What it hands
 *   over (take()) glides through those lifted stretches instead, from where the finger lifted
 *   to where the recording goes on (around the loop line), gate open, so the loop never stands
 *   still; the take itself keeps them lifted, so a later stroke splits a stretch cleanly.
 *
 * A take never touched is Cancelled. take() is valid from a Revision or Finished until the
 * next start(). Real-time safe after prepare().
 */
class TANH_API MotionTakeRecorder {
public:
    enum class Start : uint8_t {
        Started,
        Refused  ///< the loop does not fit at the minimum resolution
    };

    enum class Event : uint8_t {
        None,
        Revision,   ///< BarsN: take() holds the loop as it is now; the take goes on
        Finished,   ///< take() holds the final loop; the take is over
        Dropped,    ///< too short: nothing kept; the take is over
        Cancelled,  ///< never touched: nothing kept; the take is over
    };

    /// Allocate the working buffer and forget any take (message thread, audio stopped).
    void prepare(const MotionRecorderConfig& config);
    [[nodiscard]] size_t buffer_bytes() const;

    [[nodiscard]] Start start(const MotionTakeStart& start) TANH_NONBLOCKING_FUNCTION;
    /// One sample: a finger is down (@p live) at (@p x, @p y); the recording clock then
    /// advances by @p beats_per_sample.
    [[nodiscard]] Event step(bool live, float x, float y, double beats_per_sample)
        TANH_NONBLOCKING_FUNCTION;
    /// disarm() / stop(): a touched take ends where it is (Finished), or is Dropped or Cancelled.
    [[nodiscard]] Event end() TANH_NONBLOCKING_FUNCTION;
    /// Forget the take (prepare() during a take).
    void abort() TANH_NONBLOCKING_FUNCTION { m_active = false; }

    [[nodiscard]] bool active() const { return m_active; }
    /// Free: 0..1 towards the next of 1, 2, 4, 8, 16 bars; BarsN: the position in the loop.
    [[nodiscard]] float progress() const;
    /// The loop of the last Revision or Finished.
    [[nodiscard]] MotionLoopView take() const;

private:
    [[nodiscard]] Event finish_free() TANH_NONBLOCKING_FUNCTION;
    [[nodiscard]] Event complete_loop(Event event) TANH_NONBLOCKING_FUNCTION;
    void write(size_t index, float x, float y, uint8_t gate) TANH_NONBLOCKING_FUNCTION;
    void seed(const MotionLoopView& seed) TANH_NONBLOCKING_FUNCTION;
    void fill_gaps() TANH_NONBLOCKING_FUNCTION;

    MotionRecorderConfig m_config;
    std::vector<float> m_x;
    std::vector<float> m_y;
    std::vector<uint8_t> m_gate;
    // BarsN: the loop as handed over, the lifted stretches filled (fill_gaps()).
    std::vector<float> m_out_x;
    std::vector<float> m_out_y;
    std::vector<uint8_t> m_out_gate;
    // The running take.
    double m_clock = 0.0;      // recording clock, beats
    double m_tick_base = 0.0;  // beat of tick 0 of this pass (Free: of the first point)
    double m_tick_step = 0.0;  // beats per point
    double m_bar_beats = 4.0;
    double m_line = 0.0;  // BarsN: beat of point 0; Free: beat of the first kept point
    // The finished loop.
    double m_length = 0.0;
    double m_anchor = 0.0;
    size_t m_num_points = 0;
    size_t m_seam = 0;
    size_t m_points_per_bar = 0;
    size_t m_capacity = 0;  // BarsN: points per loop; Free: 16 bars
    size_t m_target = 0;    // Free: progress target
    size_t m_count = 0;     // Free: points written; BarsN: ticks into this pass
    float m_last_x = 0.0f;
    float m_last_y = 0.0f;
    bool m_active = false;
    bool m_bars = false;
    bool m_touched = false;
    bool m_full = false;  // BarsN: every point holds motion
    bool m_last_live = false;
};

}  // namespace thl::modulation::detail
