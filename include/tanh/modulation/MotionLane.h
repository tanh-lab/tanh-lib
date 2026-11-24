#pragma once

#include <tanh/core/AbiNamespace.h>
#include <tanh/core/Exports.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <vector>

namespace thl::inline THL_ABI_NAMESPACE::modulation {

/// Unit of a MotionLane's length, anchor and rate.
enum class MotionTimebase : uint8_t {
    Seconds,  ///< runs on its own clock (a loaded lane; takes are Beats)
    Beats     ///< quarter notes; follows the transport tempo
};

/// Default point capacity of one take (MotionRecorderConfig::m_max_points).
inline constexpr size_t k_motion_default_max_points = 32768;

/// Most channels a motion recorder and its lanes carry (MotionRecorderConfig::m_num_channels).
inline constexpr size_t k_motion_max_channels = 8;

/// One value per channel, in [0, 1]; only the first num_channels are used.
using MotionValues = std::array<float, k_motion_max_channels>;

/// One evaluated lane position.
struct MotionPoint {
    MotionValues m_values{};
    uint8_t m_gate = 0;
};

/**
 * @brief A recorded gesture: one or more channels and a gate on a uniform, looping grid.
 *
 * A channel is one value of the gesture in [0, 1]: a slider is one channel, an XY pad two
 * (x, y), and any other control as many as it has (up to k_motion_max_channels).
 * A plain value type: MotionRecorder publishes finished takes as MotionLane,
 * presets save and load it as JSON. Point i sits at i * m_length / num_points()
 * seconds or beats, whatever m_rate says. The channels are interpolated with
 * Catmull-Rom across the loop seam; the gate is a step channel. Index 0 of a
 * Beats lane plays at the beats congruent to m_anchor modulo m_length.
 */
struct TANH_API MotionLane {
    uint32_t m_take_id = 0;  ///< 0 = never published; otherwise monotonic per recorder
    MotionTimebase m_timebase = MotionTimebase::Seconds;
    double m_rate = 200.0;  ///< points per second or per beat when recorded (informational)
    double m_length = 0.0;  ///< loop length in seconds or beats
    double m_anchor = 0.0;  ///< Beats: index 0 sits at beat ≡ anchor (mod length)
    /// num_channels() channels of num_points() values each, in [0, 1].
    std::vector<std::vector<float>> m_channels;
    std::vector<uint8_t> m_gate;  ///< 0 / 1 per point
    /// Index of the first recorded point: the take's seam sits between
    /// m_seam - 1 and m_seam (not 0 for a bar take started mid-loop).
    size_t m_seam = 0;

    [[nodiscard]] size_t num_channels() const { return m_channels.size(); }
    [[nodiscard]] size_t num_points() const {
        return m_channels.empty() ? 0 : m_channels.front().size();
    }
    [[nodiscard]] bool empty() const { return num_points() == 0; }

    /// Evaluate the lane at @p phase (seconds or beats; wrapped into the loop).
    [[nodiscard]] MotionPoint sample(double phase) const;

    /// Serialise as version 2: every channel quantised to a uint16 array in "channels", the
    /// gate as [index, value] edges, plus take_id, timebase, rate, length, anchor and, when
    /// not 0, seam.
    [[nodiscard]] nlohmann::json to_json() const;

    /**
     * @brief Parse and validate a lane; never throws.
     *
     * Reads version 2 and version 1 (an XY lane: "x" and "y" become two channels).
     * Rejects (nullopt) another version, a missing or non-finite field, rate or
     * length ≤ 0 (with points), more than k_motion_max_channels, channels of different sizes
     * or out of the uint16 range, and gate
     * edges that are unsorted, out of range or not 0/1, and a seam outside the
     * points. A missing seam is 0. A lane with more than
     * @p max_points points is resampled down to @p max_points. An empty lane
     * (no points) parses to an empty MotionLane.
     */
    [[nodiscard]] static std::optional<MotionLane> from_json(
        const nlohmann::json& json,
        size_t max_points = k_motion_default_max_points);

    /// Resample to exactly @p num_points points (Catmull-Rom for the channels, step for
    /// the gate); the seam index is scaled along.
    void resample(size_t num_points);
};

}  // namespace thl::inline THL_ABI_NAMESPACE::modulation
