#pragma once

#include <tanh/core/Exports.h>
#include <tanh/modulation/detail/MotionGlide.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace thl::modulation::detail {

/**
 * @brief A recorder's output: the live touch over playback over the last value.
 *
 * A touch always wins (gate 1). Without one, playback plays (its gate), gliding from where the
 * output was after a release or when playback asks for it (a new lane, a jump); without
 * playback the output holds with the gate closed. Collects the block's change points: the
 * caller's (render ticks, input events) plus every touch and gate edge and glide start.
 * Audio thread only; prepare() allocates.
 */
class TANH_API MotionCompositor {
public:
    void prepare(double sample_rate, double glide_ms, size_t max_block);

    /// Start a block; @p glide: playback asks to glide to it from where the output is.
    void begin(bool glide) TANH_NONBLOCKING_FUNCTION;
    /// Playback asks to glide to it from the next sample (it changed mid-block).
    void glide() TANH_NONBLOCKING_FUNCTION { m_glide_pending = true; }
    /// Sample @p i (in order): the touch (@p live at @p lx, @p ly), playback (@p playing at
    /// @p px, @p py with gate @p pg); @p change: the caller's change point.
    void sample(uint32_t i,
                bool live,
                float lx,
                float ly,
                bool playing,
                float px,
                float py,
                uint8_t pg,
                bool change) TANH_NONBLOCKING_FUNCTION;

    [[nodiscard]] const float* x() const { return m_x.data(); }
    [[nodiscard]] const float* y() const { return m_y.data(); }
    [[nodiscard]] const uint8_t* gate() const { return m_gate.data(); }
    [[nodiscard]] const uint8_t* live() const { return m_live.data(); }
    [[nodiscard]] std::span<const uint32_t> change_points() const {
        return {m_change_points.data(), m_num_change_points};
    }
    /// The output's last value (where a lifted take starts, where a glide starts from).
    [[nodiscard]] float last_x() const { return m_last_x; }
    [[nodiscard]] float last_y() const { return m_last_y; }
    [[nodiscard]] bool last_live() const { return m_last_live; }

private:
    MotionGlide m_glide;
    std::vector<float> m_x;
    std::vector<float> m_y;
    std::vector<uint8_t> m_gate;
    std::vector<uint8_t> m_live;
    std::vector<uint32_t> m_change_points;
    size_t m_num_change_points = 0;
    float m_last_x = 0.0f;
    float m_last_y = 0.0f;
    uint8_t m_last_gate = 0;
    bool m_last_live = false;
    bool m_glide_pending = false;
};

}  // namespace thl::modulation::detail
