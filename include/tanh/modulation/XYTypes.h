#pragma once

// Types of the XY touch path shared by XYController, its pad and its consumers.

#include <cstddef>
#include <cstdint>

namespace thl::modulation {

/// Identifies one finger for its lifetime (a JUCE mouse source index, a UITouch
/// pointer, a React Native touch identifier).
using TouchId = uint64_t;

/// Voices (dots) per controller.
inline constexpr uint32_t k_xy_controller_max_voices = 16;
/// Fingers tracked at once per controller.
inline constexpr uint32_t k_xy_controller_max_touches = 16;

/// Which of several fingers on one voice drives it.
enum class MonoPriority : uint8_t {
    Last,  ///< the most recent touch; releasing it falls back to the newest remaining one
    First  ///< the oldest touch; later touches only take over when it is released
};

/// The three matrix sources of a voice.
enum class XYPadAxis : uint8_t { X, Y, Active };

/// Which layer produced a voice's output. Touch has priority over Motion.
enum class XYLayer : uint8_t {
    None,    ///< no layer is active: x and y hold
    Touch,   ///< a live touch
    Motion,  ///< motion playback
};

}  // namespace thl::modulation
