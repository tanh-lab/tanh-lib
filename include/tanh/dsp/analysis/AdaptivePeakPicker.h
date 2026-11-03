// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <tanh/core/Exports.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace thl::dsp::analysis {

struct TANH_API AdaptivePeakPickResult {
    std::vector<std::size_t> peak_indices;
    std::vector<double> threshold;
};

TANH_API AdaptivePeakPickResult adaptive_peak_pick(
    std::span<const double> values,
    std::size_t local_max_history_frames,
    std::size_t threshold_history_frames,
    double threshold_delta,
    std::size_t minimum_spacing_frames,
    std::size_t lookahead_frames = 1,
    std::span<const std::uint8_t> eligible = {});

}  // namespace thl::dsp::analysis
