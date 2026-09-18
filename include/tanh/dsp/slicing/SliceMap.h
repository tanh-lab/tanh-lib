#pragma once

#include <tanh/dsp/sampler/LoopRegion.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace thl::dsp::slicing {

/// Most slices a SliceMap holds.
inline constexpr size_t k_max_slices = 64;

/**
 * @brief Slice boundaries of one sample, in frames.
 *
 * `m_bounds[0] == 0`, `m_bounds[m_count] == m_total_frames`, strictly
 * ascending; slice k is `[m_bounds[k], m_bounds[k + 1])`. Trivially
 * copyable with a fixed capacity, so a host can hand a whole map to the
 * audio thread by value (seqlock, atomic swap of a copy, message queue).
 *
 * The map belongs to a sample length, not to a sample: alternative sources
 * of the same length (pitch-shifted copies, round-robins) share it, and
 * boundary_frame() rescales for a source of another length.
 */
struct SliceMap {
    uint32_t m_count{0};  ///< Number of slices; 0 = no map.
    uint64_t m_total_frames{0};
    std::array<uint64_t, k_max_slices + 1> m_bounds{};

    /// Structurally sound: 1..k_max_slices strictly ascending slices that
    /// cover the whole sample.
    [[nodiscard]] bool valid() const {
        if (m_count < 1 || m_count > k_max_slices || m_total_frames == 0) { return false; }
        if (m_bounds[0] != 0 || m_bounds[m_count] != m_total_frames) { return false; }
        for (size_t k = 0; k < m_count; ++k) {
            if (!(m_bounds[k] < m_bounds[k + 1])) { return false; }
        }
        return true;
    }

    /// Boundary k (0..m_count) as a frame of a source `total_frames` long
    /// (exact when the lengths match, rounded otherwise).
    [[nodiscard]] size_t boundary_frame(size_t k, size_t total_frames) const {
        k = std::min(k, static_cast<size_t>(m_count));
        if (m_total_frames == total_frames || m_total_frames == 0) {
            return std::min(static_cast<size_t>(m_bounds[k]), total_frames);
        }
        double const scaled = static_cast<double>(m_bounds[k]) * static_cast<double>(total_frames) /
                              static_cast<double>(m_total_frames);
        return std::min(static_cast<size_t>(std::llround(scaled)), total_frames);
    }

    /// Boundary k as a fraction of the sample.
    [[nodiscard]] float normalized_bound(size_t k) const {
        if (m_total_frames == 0) { return 0.0f; }
        k = std::min(k, static_cast<size_t>(m_count));
        return static_cast<float>(static_cast<double>(m_bounds[k]) /
                                  static_cast<double>(m_total_frames));
    }

    /// All boundaries as fractions of the sample (allocates).
    [[nodiscard]] std::vector<float> normalized_bounds() const {
        std::vector<float> out(m_count + 1u);
        for (size_t k = 0; k <= m_count; ++k) { out[k] = normalized_bound(k); }
        return out;
    }

    /// `count` equal slices of a sample `total_frames` long.
    static SliceMap grid(size_t count, size_t total_frames) {
        SliceMap map;
        map.m_count = static_cast<uint32_t>(std::clamp<size_t>(count, 1, k_max_slices));
        map.m_total_frames = total_frames;
        for (size_t k = 0; k <= map.m_count; ++k) {
            float const norm = static_cast<float>(k) / static_cast<float>(map.m_count);
            map.m_bounds[k] = to_frame(norm, total_frames);
        }
        map.m_bounds[map.m_count] = total_frames;
        return map;
    }

    /**
     * @brief Boundaries given as fractions of the sample.
     *
     * Each is rounded to the nearest frame. The result is not validated:
     * check valid() (close fractions can collapse onto the same frame).
     */
    static SliceMap from_normalized(std::span<const float> bounds, size_t total_frames) {
        SliceMap map;
        if (bounds.size() < 2) { return map; }
        map.m_count = static_cast<uint32_t>(std::min(bounds.size() - 1, k_max_slices));
        map.m_total_frames = total_frames;
        for (size_t k = 0; k <= map.m_count; ++k) {
            map.m_bounds[k] = to_frame(bounds[k], total_frames);
        }
        return map;
    }

    /// Boundaries given in frames (count = size - 1, capped). Not validated.
    static SliceMap from_frames(std::span<const size_t> bounds, size_t total_frames) {
        SliceMap map;
        if (bounds.size() < 2) { return map; }
        map.m_count = static_cast<uint32_t>(std::min(bounds.size() - 1, k_max_slices));
        map.m_total_frames = total_frames;
        for (size_t k = 0; k <= map.m_count; ++k) { map.m_bounds[k] = bounds[k]; }
        return map;
    }

private:
    static uint64_t to_frame(float norm, size_t total_frames) {
        double const f = static_cast<double>(norm) * static_cast<double>(total_frames);
        return std::min(static_cast<uint64_t>(std::max<long long>(0, std::llround(f))),
                        static_cast<uint64_t>(total_frames));
    }
};

/**
 * @brief Validate normalised boundaries a host received (UI edit, preset).
 *
 * Checks `min_count..max_count` slices, first 0, last 1, strictly ascending
 * and, when `duration_seconds > 0`, every slice at least `min_slice_seconds`
 * long (with 0.1 % slack for float round trips).
 */
inline bool valid_normalized_bounds(std::span<const float> bounds,
                                    double duration_seconds,
                                    double min_slice_seconds,
                                    size_t min_count = 1,
                                    size_t max_count = k_max_slices) {
    if (bounds.size() < min_count + 1 || bounds.size() > max_count + 1) { return false; }
    if (bounds.front() != 0.0f || bounds.back() != 1.0f) { return false; }
    double const min_seconds = min_slice_seconds * (1.0 - 1e-3);
    for (size_t i = 1; i < bounds.size(); ++i) {
        float const a = bounds[i - 1];
        float const b = bounds[i];
        if (!(b > a)) { return false; }
        if (duration_seconds > 0.0 &&
            (static_cast<double>(b) - static_cast<double>(a)) * duration_seconds < min_seconds) {
            return false;
        }
    }
    return true;
}

}  // namespace thl::dsp::slicing
