#pragma once

#include <tanh/core/Exports.h>

#include <cstdint>

namespace thl::modulation {

/// What a MotionTransform does with one setting.
struct MotionTransformInfo {
    bool m_active = false;       ///< changes what plays; inactive, the lane plays as it is
    bool m_uses_lane = false;    ///< places the lane's points: sounds only with a lane
    bool m_plays_alone = false;  ///< sounds without a lane, looping over the playback length
    bool m_opens_gate = false;   ///< the gate stays open while it sounds
    /// Changes when the path itself changes (not its amount): playback re-seeks and glides.
    uint32_t m_curve = 0;
};

/**
 * @brief Moves what a MotionRecorder plays, point by point over the loop.
 *
 * The application's shapes, scaling or any other path: the recorder passes it the position
 * in the loop and the lane's point there and plays what it returns. The transform itself is
 * stateless; its setting is a 64-bit word the application packs
 * (MotionRecorder::set_transform_settings()), so one load gives a whole, consistent setting
 * on any thread. Both methods are called on the audio thread (and on the message thread to
 * draw): real-time safe, no state, no allocation.
 */
class TANH_API MotionTransform {
public:
    MotionTransform() = default;
    virtual ~MotionTransform() = default;
    MotionTransform(const MotionTransform&) = delete;
    MotionTransform& operator=(const MotionTransform&) = delete;
    MotionTransform(MotionTransform&&) = delete;
    MotionTransform& operator=(MotionTransform&&) = delete;

    /// What @p settings do.
    [[nodiscard]] virtual MotionTransformInfo info(uint64_t settings) const = 0;
    /// The point that plays at @p u (0..1 of the loop) where the lane is at (@p x, @p y), the
    /// pad centre without a lane; written back into @p x and @p y, in [0, 1].
    virtual void place(uint64_t settings, double u, float& x, float& y) const = 0;
};

}  // namespace thl::modulation
