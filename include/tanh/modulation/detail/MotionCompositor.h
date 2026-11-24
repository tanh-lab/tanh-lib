#pragma once

#include <tanh/core/AbiNamespace.h>
#include <tanh/core/Exports.h>
#include <tanh/modulation/MotionLane.h>
#include <tanh/modulation/detail/MotionGlide.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace thl::inline THL_ABI_NAMESPACE::modulation::detail {

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
    void prepare(double sample_rate, double glide_ms, size_t max_block, size_t num_channels);

    /// Start a block; @p glide: playback asks to glide to it from where the output is.
    void begin(bool glide) TANH_NONBLOCKING_FUNCTION;
    /// Playback asks to glide to it from the next sample (it changed mid-block).
    void glide() TANH_NONBLOCKING_FUNCTION { m_glide_pending = true; }
    /// Sample @p i (in order): the touch (@p live at @p touch), playback (@p playing at
    /// @p played with gate @p pg); @p change: the caller's change point. Only the first
    /// num_channels() values are read.
    void sample(uint32_t i,
                bool live,
                const MotionValues& touch,
                bool playing,
                const MotionValues& played,
                uint8_t pg,
                bool change) TANH_NONBLOCKING_FUNCTION;

    [[nodiscard]] size_t num_channels() const { return m_num_channels; }
    /// The block's values of @p channel (< num_channels()).
    [[nodiscard]] const float* values(size_t channel) const {
        return m_values.data() + (channel * m_max_block);
    }
    [[nodiscard]] const uint8_t* gate() const { return m_gate.data(); }
    [[nodiscard]] const uint8_t* live() const { return m_live.data(); }
    [[nodiscard]] std::span<const uint32_t> change_points() const {
        return {m_change_points.data(), m_num_change_points};
    }
    /// The output's last value (where a lifted take starts, where a glide starts from).
    [[nodiscard]] const MotionValues& last() const { return m_last; }
    [[nodiscard]] bool last_live() const { return m_last_live; }

private:
    MotionGlide m_glide;
    std::vector<float> m_values;  // channel c at c * m_max_block
    std::vector<uint8_t> m_gate;
    std::vector<uint8_t> m_live;
    std::vector<uint32_t> m_change_points;
    size_t m_num_change_points = 0;
    size_t m_max_block = 0;
    size_t m_num_channels = 1;
    MotionValues m_last{};
    uint8_t m_last_gate = 0;
    bool m_last_live = false;
    bool m_glide_pending = false;
};

}  // namespace thl::inline THL_ABI_NAMESPACE::modulation::detail
