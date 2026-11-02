#pragma once

#include <tanh/core/AbiNamespace.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <cstdint>

namespace thl::inline THL_ABI_NAMESPACE::dsp::dynamics {

/// Which way a dynamics processor changes the level, relative to its threshold.
enum class DynamicsMode : uint8_t {
    DownwardCompression,  ///< above the threshold, lowered towards it (a compressor)
    UpwardCompression,    ///< below the threshold, raised towards it (quiet passages lifted)
    DownwardExpansion,    ///< below the threshold, lowered away from it (an expander, a gate)
    UpwardExpansion,      ///< above the threshold, raised away from it (transients enlarged)
};

/**
 * @brief The static curve of a dynamics processor: the gain in dB for a level in dB.
 *
 * With d the distance past the threshold in the mode's direction (above it for the downward
 * compression and upward expansion, below it otherwise) and the slope s = 1 - 1 / ratio for
 * compression, ratio - 1 for expansion, the gain is s·d for d >= knee / 2, s·(d + knee / 2)² /
 * (2·knee) inside the knee and 0 below it (the quadratic soft knee of Giannoulis, Massberg and
 * Reiss, "Digital Dynamic Range Compressor Design", JAES 2012), continuous with its slope at both
 * knee edges. The gain is negative for the downward modes and positive for the upward ones, and
 * its size is capped at the range.
 *
 * A ratio of infinity compresses to the threshold (limiting); ratios below 1 count as 1 (no
 * change). Real-time safe.
 */
struct GainComputer {
    float m_threshold_db = -18.0f;
    float m_ratio = 4.0f;
    float m_knee_db = 6.0f;
    float m_range_db = 60.0f;  ///< the largest gain change, in either direction
    DynamicsMode m_mode = DynamicsMode::DownwardCompression;

    /// The gain in dB at the level @p level_db.
    [[nodiscard]] float gain_db(float level_db) const TANH_NONBLOCKING_FUNCTION {
        const bool compression = m_mode == DynamicsMode::DownwardCompression ||
                                 m_mode == DynamicsMode::UpwardCompression;
        const bool above =
            m_mode == DynamicsMode::DownwardCompression || m_mode == DynamicsMode::UpwardExpansion;
        const bool downward = m_mode == DynamicsMode::DownwardCompression ||
                              m_mode == DynamicsMode::DownwardExpansion;
        const float ratio = std::max(m_ratio, 1.0f);
        const float slope = compression ? 1.0f - (1.0f / ratio) : ratio - 1.0f;
        const float distance = above ? level_db - m_threshold_db : m_threshold_db - level_db;
        const float knee = std::max(m_knee_db, 0.0f);
        float amount = 0.0f;
        if (2.0f * distance >= knee) {
            // distance > 0 keeps an infinite expansion slope from meeting 0 at a hard knee
            amount = distance > 0.0f ? slope * distance : 0.0f;
        } else if (2.0f * distance > -knee) {  // inside the knee (knee > 0 here)
            const float x = distance + (0.5f * knee);
            amount = slope * x * x / (2.0f * knee);
        }
        amount = std::min(amount, std::max(m_range_db, 0.0f));
        return downward ? -amount : amount;
    }
};

}  // namespace thl::inline THL_ABI_NAMESPACE::dsp::dynamics
