#pragma once

// What an XYController publishes for a UI: per-block frames and the live trail.

#include <tanh/modulation/MotionTypes.h>
#include <tanh/modulation/XYTypes.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace thl::modulation {

/// Capacity of the live recording trail (audio to UI, drop-oldest).
inline constexpr size_t k_xy_trail_capacity = 2048;

/// One live recording trail point.
struct XYPathPoint {
    float m_x = 0.0f;
    float m_y = 0.0f;
    float m_progress = 0.0f;  ///< recording progress of the voice, 0..1
    /// Where on the take's loop the point lies, 0..1 from the loop line (a BarsN take; the
    /// progress of a free take).
    float m_position = 0.0f;
    uint8_t m_voice = 0;
    uint8_t m_active = 0;
    uint8_t m_live = 0;  ///< a finger is down: the take records this point
};

/// One voice in an XYFrame. All fields come from the same audio block.
struct XYVoiceFrame {
    float m_x = 0.0f;  ///< output at block end
    float m_y = 0.0f;
    uint8_t m_active = 0;      ///< gate output, including the latch
    bool m_has_value = false;  ///< touched or played at least once
    bool m_touched = false;    ///< a live touch drives the voice
    XYLayer m_layer = XYLayer::None;
    uint32_t m_path_version = 0;  ///< MotionRecorder::lane_version(): re-read the path on change
    MotionSnapshot m_motion;      ///< the voice's recorder
};

/// Fixed-size UI snapshot of one controller, published once per block (rate
/// limited) through a TripleBuffer.
struct XYFrame {
    static constexpr uint32_t k_max_voices = k_xy_controller_max_voices;
    uint64_t m_sequence = 0;     ///< publication counter (0 = never published)
    uint64_t m_sample_time = 0;  ///< samples driven since prepare(), at block end
    uint32_t m_num_voices = 0;
    uint32_t m_trail_dropped = 0;  ///< trail points lost to a slow UI (cumulative)
    bool m_latch = false;
    std::array<XYVoiceFrame, k_max_voices> m_voices{};
};
static_assert(std::is_trivially_copyable_v<XYFrame>);

}  // namespace thl::modulation
