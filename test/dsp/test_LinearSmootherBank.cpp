#include <gtest/gtest.h>

#include <array>

#include <tanh/dsp/utils/LinearSmootherBank.h>

using thl::dsp::utils::LinearSmootherBank;

TEST(LinearSmootherBank, RampsMultipleLanesIndependently) {
    LinearSmootherBank smoothers;
    smoothers.resize(3);
    smoothers.set_ramp_samples(4);
    smoothers.set_current_and_target(0, 0.0f);
    smoothers.set_current_and_target(1, 10.0f);
    smoothers.set_current_and_target(2, -4.0f);

    smoothers.set_target(0, 1.0f);
    smoothers.set_target(1, 6.0f);

    EXPECT_FLOAT_EQ(smoothers.next(0), 0.25f);
    EXPECT_FLOAT_EQ(smoothers.next(1), 9.0f);
    EXPECT_FLOAT_EQ(smoothers.next(2), -4.0f);

    EXPECT_FLOAT_EQ(smoothers.next(0), 0.5f);
    EXPECT_FLOAT_EQ(smoothers.next(1), 8.0f);
    EXPECT_EQ(smoothers.samples_remaining(0), 2u);
    EXPECT_EQ(smoothers.samples_remaining(1), 2u);

    smoothers.next(0);
    EXPECT_FLOAT_EQ(smoothers.next(0), 1.0f);
    EXPECT_EQ(smoothers.samples_remaining(0), 0u);

    smoothers.next(1);
    EXPECT_FLOAT_EQ(smoothers.next(1), 6.0f);
    EXPECT_EQ(smoothers.samples_remaining(1), 0u);
}

TEST(LinearSmootherBank, ZeroRampSnapsToTarget) {
    LinearSmootherBank smoothers;
    smoothers.resize(1);
    smoothers.set_ramp_samples(0);
    smoothers.set_current_and_target(0, 2.0f);

    smoothers.set_target(0, 8.0f);

    EXPECT_FLOAT_EQ(smoothers.current(0), 8.0f);
    EXPECT_FLOAT_EQ(smoothers.next(0), 8.0f);
    EXPECT_EQ(smoothers.samples_remaining(0), 0u);
}

TEST(LinearSmootherBank, AdvancesAllRequestedLanes) {
    LinearSmootherBank smoothers;
    smoothers.resize(3);
    smoothers.set_ramp_samples(2);
    smoothers.set_target(0, 2.0f);
    smoothers.set_target(1, 4.0f);
    smoothers.set_target(2, 6.0f);

    std::array<float, 3> output{};
    smoothers.next_all(output.data(), output.size());

    EXPECT_FLOAT_EQ(output[0], 1.0f);
    EXPECT_FLOAT_EQ(output[1], 2.0f);
    EXPECT_FLOAT_EQ(output[2], 3.0f);
}
