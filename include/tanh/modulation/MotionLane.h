#pragma once

#include <tanh/core/Exports.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <vector>

namespace thl::modulation {

/// Unit of a MotionLane's length, anchor and rate.
enum class MotionTimebase : uint8_t {
    Seconds,  ///< free take recorded without a running transport
    Beats     ///< quarter notes; follows the transport tempo
};

/// Default point capacity of one take (MotionRecorderConfig::m_max_points).
inline constexpr size_t k_motion_default_max_points = 32768;

/// One evaluated lane position.
struct MotionPoint {
    float m_x = 0.0f;
    float m_y = 0.0f;
    uint8_t m_gate = 0;
};

namespace detail {

/// Wrap @p value into [0, length). Returns 0 for length <= 0 or a non-finite value.
[[nodiscard]] inline double wrap_phase(double value, double length) {
    if (!(length > 0.0) || !std::isfinite(value)) { return 0.0; }
    double p = value - (std::floor(value / length) * length);
    if (p >= length || p < 0.0) { p = 0.0; }
    return p;
}

/// Uniform Catmull-Rom through @p n looping points at the fractional @p index,
/// clamped to [0, 1]. Returns 0 for n == 0.
[[nodiscard]] float catmull_rom_wrap(const float* points, size_t n, double index);

}  // namespace detail

/**
 * @brief A recorded XY gesture: x, y and a gate on a uniform, looping grid.
 *
 * A plain value type: MotionRecorder publishes finished takes as MotionLane,
 * presets save and load it as JSON. Point i sits at i * m_length / num_points()
 * seconds or beats, whatever m_rate says. x and y are interpolated with
 * Catmull-Rom across the loop seam; the gate is a step channel. Index 0 of a
 * Beats lane plays at the beats congruent to m_anchor modulo m_length.
 */
struct TANH_API MotionLane {
    uint32_t m_take_id = 0;  ///< 0 = never published; otherwise monotonic per recorder
    MotionTimebase m_timebase = MotionTimebase::Seconds;
    double m_rate = 200.0;        ///< points per second or per beat when recorded (informational)
    double m_length = 0.0;        ///< loop length in seconds or beats
    double m_anchor = 0.0;        ///< Beats: index 0 sits at beat ≡ anchor (mod length)
    std::vector<float> m_x;       ///< [0, 1], num_points() entries
    std::vector<float> m_y;       ///< [0, 1], num_points() entries
    std::vector<uint8_t> m_gate;  ///< 0 / 1 per point

    [[nodiscard]] size_t num_points() const { return m_x.size(); }
    [[nodiscard]] bool empty() const { return m_x.empty(); }

    /// Evaluate the lane at @p phase (seconds or beats; wrapped into the loop).
    [[nodiscard]] MotionPoint sample(double phase) const;

    /// Serialise as version 1: x and y quantised to uint16 arrays, the gate as
    /// [index, value] edges, plus take_id, timebase, rate, length and anchor.
    [[nodiscard]] nlohmann::json to_json() const;

    /**
     * @brief Parse and validate a lane; never throws.
     *
     * Rejects (nullopt) a wrong version, a missing or non-finite field, rate or
     * length ≤ 0, x/y of different sizes or out of the uint16 range, and gate
     * edges that are unsorted, out of range or not 0/1. A lane with more than
     * @p max_points points is resampled down to @p max_points. An empty lane
     * (no points) parses to an empty MotionLane.
     */
    [[nodiscard]] static std::optional<MotionLane> from_json(
        const nlohmann::json& json,
        size_t max_points = k_motion_default_max_points);

    /// Resample to exactly @p num_points points (Catmull-Rom for x/y, step for the gate).
    void resample(size_t num_points);
};

}  // namespace thl::modulation
