#pragma once

#include <tanh/core/Exports.h>
#include <tanh/modulation/MotionTransform.h>
#include <tanh/modulation/MotionTypes.h>
#include <tanh/modulation/detail/MotionClock.h>
#include <tanh/modulation/detail/MotionLaneStore.h>
#include <tanh/modulation/detail/MotionTakeRecorder.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <cstdint>

namespace thl::modulation::detail {

/// How a block plays, besides what plays.
struct MotionPlaySettings {
    LoopLength m_length = LoopLength::Free;  ///< BarsN: every lane stretched onto N bars
    bool m_reverse = false;
    int m_sig_num = 4;
    int m_sig_denom = 4;
    const MotionTransform* m_transform = nullptr;
    uint64_t m_transform_settings = 0;
};

/**
 * @brief Plays a lane (or a transform without one) in sync with the recording clock.
 *
 * Beats loops take their phase from the clock's beat: point 0 at the beats congruent to the
 * anchor, every block. Seconds loops run on their own from where they were. The transport's
 * flags alone decide what a difference to the running phase means: none, rounding (followed
 * silently); a timeline reset, a re-lock; a jump or a start, a re-lock that glides when the
 * phase moved by more than two points. Catmull-Rom is evaluated every render interval and
 * ramped linearly in between, the grid restarting at every loop wrap.
 *
 * begin() reports whether the output should glide to playback (a new lane or curve, a new
 * length, reverse, a jump): the caller owns the output and its glide. Audio thread only.
 */
class TANH_API MotionLanePlayer {
public:
    void prepare(double sample_rate, uint32_t render_interval);
    /// The next block re-locks to its beat, without a glide.
    void reset() TANH_NONBLOCKING_FUNCTION;
    /// The next block re-locks to its beat and glides (playback enabled again).
    void restart() TANH_NONBLOCKING_FUNCTION;

    /// Start a block playing @p source: true if the output should glide to playback.
    [[nodiscard]] bool begin(const MotionBlockClock& clock,
                             const MotionLaneSource& source,
                             const MotionPlaySettings& settings) TANH_NONBLOCKING_FUNCTION;
    /// False when nothing plays this block (gate 0, the output holds).
    [[nodiscard]] bool playing() const { return !m_view.empty(); }
    /// The played point at offset @p i (in order, every sample of the block); true at a render
    /// tick (a change point of the output).
    bool render(uint32_t i, float& x, float& y, uint8_t& gate) TANH_NONBLOCKING_FUNCTION;
    /// End the block of @p num_samples.
    void end(uint32_t num_samples) TANH_NONBLOCKING_FUNCTION;

    /// Phase at the end of the last block, in seconds or beats into the loop.
    [[nodiscard]] double phase() const { return m_phase; }
    /// Loop length of what played in the last block; 0: nothing.
    [[nodiscard]] double length() const { return m_length; }
    [[nodiscard]] MotionTimebase timebase() const { return m_timebase; }
    /// Take id of what played in the last block; 0: nothing.
    [[nodiscard]] uint32_t take_id() const { return m_playing_id; }
    /// Glides started by a flagged jump since prepare().
    [[nodiscard]] uint64_t jump_glides() const { return m_jump_glides; }

private:
    // What plays in a block: the lane's points (maybe stretched) and the transform over them.
    struct View {
        const float* m_x = nullptr;
        const float* m_y = nullptr;
        const uint8_t* m_gate = nullptr;
        size_t m_num_points = 0;
        MotionTimebase m_timebase = MotionTimebase::Seconds;
        double m_length = 0.0;
        double m_anchor = 0.0;
        bool m_transformed = false;
        bool m_open_gate = false;
        [[nodiscard]] bool empty() const { return m_num_points == 0 || !(m_length > 0.0); }
    };

    [[nodiscard]] View view_of(const MotionLaneSource& source,
                               const MotionPlaySettings& settings,
                               const MotionTransformInfo& info) const TANH_NONBLOCKING_FUNCTION;
    [[nodiscard]] double clock_phase(double beat) const TANH_NONBLOCKING_FUNCTION;
    void evaluate(double phase, float& x, float& y) const TANH_NONBLOCKING_FUNCTION;

    double m_sample_rate = 48000.0;
    uint32_t m_interval = 32;
    View m_view;
    const MotionTransform* m_transform = nullptr;
    uint64_t m_transform_settings = 0;
    MotionBlockClock m_clock;
    double m_step = 0.0;  // phase per sample this block
    double m_segment_phase = 0.0;
    double m_phase = 0.0;
    double m_length = 0.0;
    double m_last_render_phase = 0.0;
    double m_previous_step = 1.0;
    uint64_t m_jump_glides = 0;
    uint32_t m_segment_start = 0;
    uint32_t m_ramp_position = 0;
    uint32_t m_playing_id = 0;
    uint32_t m_version = 0;
    uint32_t m_curve = 0;
    float m_ramp_from_x = 0.0f;
    float m_ramp_from_y = 0.0f;
    float m_ramp_to_x = 0.0f;
    float m_ramp_to_y = 0.0f;
    MotionTimebase m_timebase = MotionTimebase::Seconds;
    LoopLength m_play_length = LoopLength::Free;
    bool m_reverse = false;
    bool m_transformed = false;
    bool m_need_phase = true;
    bool m_glide_next = false;
    bool m_ramp_restart = true;
};

}  // namespace thl::modulation::detail
