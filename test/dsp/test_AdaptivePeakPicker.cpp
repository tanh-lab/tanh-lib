#include <gtest/gtest.h>

#include <tanh/dsp/analysis/AdaptivePeakPicker.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace {

using thl::dsp::analysis::adaptive_peak_pick;

TEST(AdaptivePeakPicker, SelectsLocalMaximaAboveAdaptiveThreshold) {
    const std::array values{0.0, 0.1, 0.8, 0.2, 0.0, 0.4, 0.9, 0.1};

    const auto result = adaptive_peak_pick(values, 2, 3, 0.1, 3, 1);

    EXPECT_EQ(result.peak_indices, (std::vector<std::size_t>{2, 6}));
    EXPECT_DOUBLE_EQ(result.threshold[2], 0.15);
    EXPECT_DOUBLE_EQ(result.threshold[6], 0.3);
}

TEST(AdaptivePeakPicker, RejectsNearbySecondaryPeak) {
    const std::array values{0.0, 0.8, 0.0, 0.7, 0.0};

    const auto result = adaptive_peak_pick(values, 1, 1, 0.1, 3, 1);

    EXPECT_EQ(result.peak_indices, (std::vector<std::size_t>{1}));
}

TEST(AdaptivePeakPicker, AppliesEligibilityMask) {
    const std::array values{0.0, 0.8, 0.0};
    const std::array<std::uint8_t, 3> eligible{1, 0, 1};

    const auto result = adaptive_peak_pick(values, 1, 1, 0.1, 1, 1, eligible);

    EXPECT_TRUE(result.peak_indices.empty());
}

TEST(AdaptivePeakPicker, SelectsFirstEligiblePointOnPlateau) {
    const std::array values{0.0, 0.8, 0.8, 0.0};

    const auto result = adaptive_peak_pick(values, 1, 2, 0.0, 2, 1);

    EXPECT_EQ(result.peak_indices, (std::vector<std::size_t>{1}));
}

TEST(AdaptivePeakPicker, EmptyInputReturnsEmptyPeaksAndThreshold) {
    const std::span<const double> values;

    const auto result = adaptive_peak_pick(values, 1, 1, 0.0, 1);

    EXPECT_TRUE(result.peak_indices.empty());
    EXPECT_TRUE(result.threshold.empty());
}

TEST(AdaptivePeakPicker, FirstThresholdIsInfinite) {
    const std::array values{0.5};

    const auto result = adaptive_peak_pick(values, 1, 1, 0.0, 1);

    EXPECT_TRUE(std::isinf(result.threshold.front()));
}

TEST(AdaptivePeakPicker, HandlesLookaheadAtMaximumSize) {
    const std::array values{0.0, 0.8};

    const auto result = adaptive_peak_pick(
        values,
        1,
        1,
        0.0,
        1,
        std::numeric_limits<std::size_t>::max());

    EXPECT_EQ(result.peak_indices, (std::vector<std::size_t>{1}));
}

TEST(AdaptivePeakPicker, RejectsNonFiniteValues) {
    const std::array values{0.0, std::numeric_limits<double>::quiet_NaN()};

    EXPECT_THROW(adaptive_peak_pick(values, 1, 1, 0.0, 1), std::invalid_argument);
}

}  // namespace
