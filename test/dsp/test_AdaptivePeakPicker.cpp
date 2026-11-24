#include <gtest/gtest.h>
#include <tanh/dsp/analysis/AdaptivePeakPicker.h>

#include <algorithm>
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

    EXPECT_EQ(result.m_peak_indices, (std::vector<std::size_t>{2, 6}));
    EXPECT_DOUBLE_EQ(result.m_threshold[2], 0.15);
    EXPECT_DOUBLE_EQ(result.m_threshold[6], 0.3);
}

TEST(AdaptivePeakPicker, RejectsNearbySecondaryPeak) {
    const std::array values{0.0, 0.8, 0.0, 0.7, 0.0};

    const auto result = adaptive_peak_pick(values, 1, 1, 0.1, 3, 1);

    EXPECT_EQ(result.m_peak_indices, (std::vector<std::size_t>{1}));
}

TEST(AdaptivePeakPicker, AppliesEligibilityMask) {
    const std::array values{0.0, 0.8, 0.0};
    const std::array<std::uint8_t, 3> eligible{1, 0, 1};

    const auto result = adaptive_peak_pick(values, 1, 1, 0.1, 1, 1, eligible);

    EXPECT_TRUE(result.m_peak_indices.empty());
}

TEST(AdaptivePeakPicker, SelectsFirstEligiblePointOnPlateau) {
    const std::array values{0.0, 0.8, 0.8, 0.0};

    const auto result = adaptive_peak_pick(values, 1, 2, 0.0, 2, 1);

    EXPECT_EQ(result.m_peak_indices, (std::vector<std::size_t>{1}));
}

TEST(AdaptivePeakPicker, FlatRegionAtItsTrailingMeanIsNoPeak) {
    const std::vector<double> silence(16, 0.0);
    std::vector<double> step_onto_plateau(16, 0.5);
    std::fill_n(step_onto_plateau.begin(), 3, 0.0);

    EXPECT_TRUE(adaptive_peak_pick(silence, 2, 4, 0.0, 3, 1).m_peak_indices.empty());
    EXPECT_EQ(adaptive_peak_pick(step_onto_plateau, 2, 4, 0.0, 3, 1).m_peak_indices,
              (std::vector<std::size_t>{3, 6}));
}

TEST(AdaptivePeakPicker, EmptyInputReturnsEmptyPeaksAndThreshold) {
    const std::span<const double> values;

    const auto result = adaptive_peak_pick(values, 1, 1, 0.0, 1);

    EXPECT_TRUE(result.m_peak_indices.empty());
    EXPECT_TRUE(result.m_threshold.empty());
}

TEST(AdaptivePeakPicker, FirstThresholdIsInfinite) {
    const std::array values{0.5};

    const auto result = adaptive_peak_pick(values, 1, 1, 0.0, 1);

    EXPECT_TRUE(std::isinf(result.m_threshold.front()));
}

TEST(AdaptivePeakPicker, HandlesLookaheadAtMaximumSize) {
    const std::array values{0.0, 0.8};

    const auto result =
        adaptive_peak_pick(values, 1, 1, 0.0, 1, std::numeric_limits<std::size_t>::max());

    EXPECT_EQ(result.m_peak_indices, (std::vector<std::size_t>{1}));
}

TEST(AdaptivePeakPicker, RejectsNonFiniteValues) {
    const std::array values{0.0, std::numeric_limits<double>::quiet_NaN()};

    EXPECT_THROW(adaptive_peak_pick(values, 1, 1, 0.0, 1), std::invalid_argument);
}

}  // namespace
