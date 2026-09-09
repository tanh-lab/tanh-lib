#pragma once

#include <tanh/core/Exports.h>

#include <array>
#include <cstddef>
#include <string_view>

namespace thl::dsp::utils {

/// The window shapes, in morph order: 0 = Rectangle ... 7 = Impulse.
enum class WindowShape : int {
    Rectangle,   // flat, only the end points at zero
    Trapezoid,   // 15 % linear ramps
    HalfCosine,  // sin(pi x)
    Triangle,
    Hann,      // 0.5 (1 - cos 2 pi x)
    Gaussian,  // sigma 0.35
    Narrow,    // Gaussian, sigma 0.18
    Impulse,   // Gaussian, sigma 0.06
    NumShapes
};

/**
 * @brief A bank of grain windows morphed by a continuous shape value and
 *        skewed by a tilt.
 *
 * `shape` runs 0 .. NumShapes - 1: an integer lands exactly on that shape,
 * a fraction is the linear blend of its two neighbours. `tilt` in [-1, 1]
 * moves the window's peak — 0 leaves it centred, +1 pushes it to the
 * end, -1 to the start — by warping time piecewise-linearly around the
 * peak, so the shape's rise and fall keep their character. The rectangle
 * (and anything within 2 % of it) is flat and ignores tilt.
 *
 * Every shape is a k_resolution-point table normalised to peak 1 with
 * both end points at 0, so a grain always starts and ends in silence.
 * `shared()` is one process-wide instance built at static initialisation:
 * no audio thread pays for the tables.
 */
class TANH_API MorphWindow {
public:
    static constexpr size_t k_num_shapes = static_cast<size_t>(WindowShape::NumShapes);
    static constexpr size_t k_resolution = 512;
    static constexpr float k_max_shape = static_cast<float>(k_num_shapes - 1);

    MorphWindow();

    /// Window value at `phase` in [0, 1) for `shape` in [0, k_max_shape] and
    /// `tilt` in [-1, 1]. Inputs are clamped.
    float at(float phase, float shape, float tilt) const;

    /// One shape, untilted, at `phase`.
    float shape_at(WindowShape shape, float phase) const;

    /// The tilt time-warp on its own: output phase -> source phase.
    static float warp(float phase, float tilt);

    static std::string_view name(WindowShape shape);

    /// The process-wide bank.
    static const MorphWindow& shared();

private:
    using Table = std::array<float, k_resolution>;
    float sample(const Table& table, float position) const;

    std::array<Table, k_num_shapes> m_tables{};
};

}  // namespace thl::dsp::utils
