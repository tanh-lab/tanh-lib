#pragma once

#include <tanh/core/AbiNamespace.h>
#include <tanh/core/Exports.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace thl::inline THL_ABI_NAMESPACE::dsp::analysis {

struct TANH_API AdaptivePeakPickResult {
    std::vector<std::size_t> m_peak_indices;
    std::vector<double> m_threshold;
};

/**
 * Picks the peaks of @p values above an adaptive threshold.
 *
 * Frame i is a peak when it is the maximum of the frames from i - @p local_max_history_frames to
 * i + @p lookahead_frames, strictly above its threshold, eligible (@p eligible is empty or non-zero
 * at i) and at least @p minimum_spacing_frames after the previous peak. Its threshold is the mean
 * of the up to @p threshold_history_frames frames before it plus @p threshold_delta. Frame 0 has no
 * frames before it, so its threshold is infinite and it is never a peak. A frame equal to its
 * threshold is no peak either, so silence does not fire with a @p threshold_delta of 0.
 *
 * @return the peak indices in ascending order and every frame's threshold.
 * @throws std::invalid_argument if a frame count other than @p lookahead_frames is 0,
 * @p threshold_delta is negative, @p eligible is neither empty nor as long as @p values, or a
 * value is not finite.
 */
TANH_API AdaptivePeakPickResult adaptive_peak_pick(std::span<const double> values,
                                                   std::size_t local_max_history_frames,
                                                   std::size_t threshold_history_frames,
                                                   double threshold_delta,
                                                   std::size_t minimum_spacing_frames,
                                                   std::size_t lookahead_frames = 1,
                                                   std::span<const std::uint8_t> eligible = {});

}  // namespace thl::inline THL_ABI_NAMESPACE::dsp::analysis
