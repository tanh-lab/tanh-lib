#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace thl::dsp::granular {

inline constexpr int k_max_slices = 16;
inline constexpr int k_min_slices = 2;

// Slice boundaries of one bank, normalised (not frames): the pitch banks in
// SampleReader are equal-length and time-aligned, so one map serves every
// bank. Strictly ascending, m_bounds[0] = 0, m_bounds[m_count] = 1. A POD of
// 68 bytes so the audio thread can copy it once per block.
//
// "Slice space": while slicing is on, Position / Start / End keep their 0..1
// range but are read as equal steps, one per slice, however long each slice
// is in time. A Position u picks slice floor(u·N); Start and End pick
// boundary round(u·N). Everything that moves those parameters (pad, LFO,
// sequencer, randomize) therefore steps through slices evenly for free.
struct SliceMap {
    int m_count{0};  // 0 = no map
    std::array<float, k_max_slices + 1> m_bounds{};

    bool valid() const {
        if (m_count < k_min_slices || m_count > k_max_slices) { return false; }
        if (m_bounds[0] != 0.0f || m_bounds[static_cast<size_t>(m_count)] != 1.0f) { return false; }
        for (int k = 0; k < m_count; ++k) {
            if (!(m_bounds[static_cast<size_t>(k)] < m_bounds[static_cast<size_t>(k) + 1])) {
                return false;
            }
        }
        return true;
    }

    // Position: the slice a step value picks. An empty map (or a NaN) picks
    // slice 0 rather than reaching undefined behaviour in the clamp.
    int slice_of_step(float u) const {
        if (m_count <= 0 || !std::isfinite(u)) { return 0; }
        auto const n = static_cast<float>(m_count);
        int const k = static_cast<int>(std::floor(std::clamp(u, 0.0f, 1.0f) * n));
        return std::clamp(k, 0, m_count - 1);
    }

    // Start / End: the boundary a step value picks (0..N).
    int boundary_of_step(float u) const {
        if (m_count <= 0 || !std::isfinite(u)) { return 0; }
        auto const n = static_cast<float>(m_count);
        int const k = static_cast<int>(std::lround(std::clamp(u, 0.0f, 1.0f) * n));
        return std::clamp(k, 0, m_count);
    }

    // Slice-space position v in [0, N] -> sample fraction, piecewise linear
    // through the boundaries (v = 2.5 is the middle of slice 3 in time).
    float step_to_norm(double v) const {
        if (m_count <= 0 || !std::isfinite(v)) { return 0.0f; }
        auto const n = static_cast<double>(m_count);
        v = std::clamp(v, 0.0, n);
        int k = static_cast<int>(std::floor(v));
        if (k >= m_count) { return 1.0f; }
        k = std::max(k, 0);
        auto const lo = static_cast<double>(m_bounds[static_cast<size_t>(k)]);
        auto const hi = static_cast<double>(m_bounds[static_cast<size_t>(k) + 1]);
        return static_cast<float>(lo + (v - static_cast<double>(k)) * (hi - lo));
    }

    size_t boundary_frame(int k, size_t total_frames) const {
        k = std::clamp(k, 0, m_count);
        auto const f = static_cast<double>(m_bounds[static_cast<size_t>(k)]) *
                       static_cast<double>(total_frames);
        return std::min(static_cast<size_t>(std::llround(f)), total_frames);
    }

    static SliceMap grid(int count) {
        SliceMap m;
        m.m_count = std::clamp(count, k_min_slices, k_max_slices);
        for (int k = 0; k <= m.m_count; ++k) {
            m.m_bounds[static_cast<size_t>(k)] =
                static_cast<float>(k) / static_cast<float>(m.m_count);
        }
        m.m_bounds[static_cast<size_t>(m.m_count)] = 1.0f;
        return m;
    }
};

}  // namespace thl::dsp::granular
