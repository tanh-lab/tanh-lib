#pragma once

#include <tanh/core/Exports.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace thl::modulation {

/**
 * @brief A stock motion path, or the recorded take.
 *
 * Every shape is a closed curve over one loop that starts at the top of the pad and turns
 * clockwise where it has a direction, so two shapes morph without collapsing through the
 * centre. The values are saved (presets, host automation as a choice index): append new
 * shapes, never reorder.
 */
enum class MotionShape : uint8_t {
    Take,        ///< the recorded lane (centre while there is none)
    Circle,      ///< circle
    Square,      ///< square, corners on the diagonals
    Triangle,    ///< triangle, point up
    Line,        ///< vertical line, up and down
    Infinity,    ///< horizontal figure eight (Lissajous 1:2)
    Knot,        ///< Lissajous 3:2
    Mesh,        ///< Lissajous 5:4
    Scan,        ///< zigzag scan (Lissajous 1:6)
    Petals3,     ///< rose, three petals
    Petals4,     ///< rose, four petals
    Petals8,     ///< rose, eight petals
    Spiral,      ///< spiral out and back in, four turns
    Heart,       ///< heart
    Star,        ///< five-pointed star
    Ellipse,     ///< flat ellipse on the diagonal
    Cloud,       ///< circle with five bumps
    Scribble,    ///< a fixed jagged path
    Spirograph,  ///< hypotrochoid, three loops
};

/// Number of MotionShape values (Take included).
inline constexpr size_t k_motion_shape_count = 19;

/// Display name of @p shape ("Take", "Circle", ...).
[[nodiscard]] TANH_API std::string_view motion_shape_name(MotionShape shape);

/// A point of a shape in [-1, 1] x [-1, 1], y up.
struct MotionShapePoint {
    float m_x = 0.0f;
    float m_y = 0.0f;
};

/// The point of @p shape at @p u, the position in the loop in [0, 1) (wrapped). Take: the centre.
[[nodiscard]] TANH_API MotionShapePoint evaluate_motion_shape(MotionShape shape,
                                                              double u) TANH_NONBLOCKING_FUNCTION;

/**
 * @brief What a MotionRecorder plays in place of, or blended with, its lane.
 *
 * Two slots, A and B, each a shape or the take, crossfaded point by point at the same
 * position in the loop by m_morph. m_size and m_rotation transform the shape slots (not the
 * take) around the pad centre. With both slots on Take nothing changes: the lane plays.
 */
struct MotionShapeMix {
    MotionShape m_a = MotionShape::Take;
    MotionShape m_b = MotionShape::Take;
    float m_morph = 0.0f;     ///< 0 = A, 1 = B
    float m_size = 1.0f;      ///< 0..1: 1 reaches the pad's edges
    float m_rotation = 0.0f;  ///< turns, clockwise (0..1)

    /// True when a shape slot is in use.
    [[nodiscard]] bool active() const {
        return m_a != MotionShape::Take || m_b != MotionShape::Take;
    }
    /// True when a slot plays the take.
    [[nodiscard]] bool uses_take() const {
        return m_a == MotionShape::Take || m_b == MotionShape::Take;
    }
};

/// Pad position, [0, 1] x [0, 1], of a shape slot at @p u: the shape scaled by m_size, turned by
/// m_rotation around the centre.
[[nodiscard]] TANH_API MotionShapePoint place_motion_shape(MotionShape shape,
                                                           const MotionShapeMix& mix,
                                                           double u) TANH_NONBLOCKING_FUNCTION;

/// Pad position of @p mix at @p u; @p take is the take's pad position there (for Take slots).
[[nodiscard]] TANH_API MotionShapePoint mix_motion_shapes(const MotionShapeMix& mix,
                                                          double u,
                                                          MotionShapePoint take)
    TANH_NONBLOCKING_FUNCTION;

}  // namespace thl::modulation
