// SPDX-License-Identifier: Apache-2.0
#include <tanh/dsp/analysis/AdaptivePeakPicker.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace thl::dsp::analysis {
namespace {

std::vector<double> trailing_mean(std::span<const double> values,
                                  std::size_t history_frames) {
    std::vector<double> cumulative_sum(values.size() + 1, 0.0);
    for (std::size_t index = 0; index < values.size(); ++index) {
        cumulative_sum[index + 1] = cumulative_sum[index] + values[index];
    }

    std::vector<double> mean(values.size(), std::numeric_limits<double>::infinity());
    for (std::size_t index = 1; index < values.size(); ++index) {
        const auto history_start = index > history_frames ? index - history_frames : 0;
        const auto history_count = index - history_start;
        const auto history_sum = cumulative_sum[index] - cumulative_sum[history_start];
        mean[index] = history_sum / static_cast<double>(history_count);
    }
    return mean;
}

}  // namespace

AdaptivePeakPickResult adaptive_peak_pick(std::span<const double> values,
                                          std::size_t local_max_history_frames,
                                          std::size_t threshold_history_frames,
                                          double threshold_delta,
                                          std::size_t minimum_spacing_frames,
                                          std::size_t lookahead_frames,
                                          std::span<const std::uint8_t> eligible) {
    if (local_max_history_frames < 1) {
        throw std::invalid_argument("local_max_history_frames must be at least one");
    }
    if (threshold_history_frames < 1) {
        throw std::invalid_argument("threshold_history_frames must be at least one");
    }
    if (threshold_delta < 0.0) {
        throw std::invalid_argument("threshold_delta cannot be negative");
    }
    if (minimum_spacing_frames < 1) {
        throw std::invalid_argument("minimum_spacing_frames must be at least one");
    }
    if (!eligible.empty() && eligible.size() != values.size()) {
        throw std::invalid_argument("eligible must have the same shape as values");
    }
    if (!std::all_of(values.begin(), values.end(), [](double value) {
            return std::isfinite(value);
        })) {
        throw std::invalid_argument("values must contain only finite values");
    }

    AdaptivePeakPickResult result;
    result.threshold = trailing_mean(values, threshold_history_frames);
    for (auto& value : result.threshold) {
        value += threshold_delta;
    }

    for (std::size_t index = 0; index < values.size(); ++index) {
        const auto history_start =
            index > local_max_history_frames ? index - local_max_history_frames : 0;
        const auto available_lookahead = values.size() - index - 1;
        const auto included_lookahead =
            std::min(available_lookahead, lookahead_frames);
        const auto lookahead_end = index + included_lookahead + 1;
        const auto local_maximum =
            *std::max_element(values.begin() + static_cast<std::ptrdiff_t>(history_start),
                              values.begin() + static_cast<std::ptrdiff_t>(lookahead_end));
        const bool is_eligible = eligible.empty() || eligible[index] != 0;
        const bool has_spacing =
            result.peak_indices.empty() ||
            index - result.peak_indices.back() >= minimum_spacing_frames;

        if (values[index] == local_maximum && values[index] >= result.threshold[index] &&
            is_eligible && has_spacing) {
            result.peak_indices.push_back(index);
        }
    }

    return result;
}

}  // namespace thl::dsp::analysis
