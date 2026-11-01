#include <gtest/gtest.h>
#include <tanh/dsp/dynamics/GainComputer.h>

#include <array>
#include <limits>

using thl::dsp::dynamics::DynamicsMode;
using thl::dsp::dynamics::GainComputer;

namespace {

GainComputer hard(DynamicsMode mode, float ratio) {
    GainComputer computer;
    computer.m_threshold_db = -20.0f;
    computer.m_ratio = ratio;
    computer.m_knee_db = 0.0f;
    computer.m_range_db = 100.0f;
    computer.m_mode = mode;
    return computer;
}

constexpr std::array<DynamicsMode, 4> k_modes{DynamicsMode::DownwardCompression,
                                              DynamicsMode::UpwardCompression,
                                              DynamicsMode::DownwardExpansion,
                                              DynamicsMode::UpwardExpansion};

}  // namespace

TEST(GainComputer, DownwardCompressionAboveTheThreshold) {
    const GainComputer computer = hard(DynamicsMode::DownwardCompression, 4.0f);
    EXPECT_FLOAT_EQ(computer.gain_db(-40.0f), 0.0f);
    EXPECT_FLOAT_EQ(computer.gain_db(-20.0f), 0.0f);
    // 12 dB over at 4:1 leaves 3 dB over: -9 dB.
    EXPECT_FLOAT_EQ(computer.gain_db(-8.0f), -9.0f);
}

TEST(GainComputer, UpwardCompressionBelowTheThreshold) {
    const GainComputer computer = hard(DynamicsMode::UpwardCompression, 2.0f);
    EXPECT_FLOAT_EQ(computer.gain_db(0.0f), 0.0f);
    // 20 dB under at 2:1 leaves 10 dB under: +10 dB.
    EXPECT_FLOAT_EQ(computer.gain_db(-40.0f), 10.0f);
}

TEST(GainComputer, DownwardExpansionBelowTheThreshold) {
    const GainComputer computer = hard(DynamicsMode::DownwardExpansion, 2.0f);
    EXPECT_FLOAT_EQ(computer.gain_db(-10.0f), 0.0f);
    // 10 dB under at 1:2 ends 20 dB under: -10 dB.
    EXPECT_FLOAT_EQ(computer.gain_db(-30.0f), -10.0f);
}

TEST(GainComputer, UpwardExpansionAboveTheThreshold) {
    const GainComputer computer = hard(DynamicsMode::UpwardExpansion, 1.5f);
    EXPECT_FLOAT_EQ(computer.gain_db(-30.0f), 0.0f);
    EXPECT_FLOAT_EQ(computer.gain_db(-10.0f), 5.0f);
}

TEST(GainComputer, InfiniteRatioLimitsAndGates) {
    const float infinity = std::numeric_limits<float>::infinity();
    EXPECT_FLOAT_EQ(hard(DynamicsMode::DownwardCompression, infinity).gain_db(-5.0f), -15.0f);
    // A gate: everything under the threshold goes down by the range.
    GainComputer gate = hard(DynamicsMode::DownwardExpansion, infinity);
    gate.m_range_db = 80.0f;
    EXPECT_FLOAT_EQ(gate.gain_db(-21.0f), -80.0f);
    EXPECT_FLOAT_EQ(gate.gain_db(-20.0f), 0.0f);
    EXPECT_FLOAT_EQ(gate.gain_db(-19.0f), 0.0f);
}

TEST(GainComputer, RatioOneOrLessChangesNothing) {
    for (const DynamicsMode mode : k_modes) {
        for (const float ratio : {1.0f, 0.5f, 0.0f, -3.0f}) {
            const GainComputer computer = hard(mode, ratio);
            for (float level = -80.0f; level <= 10.0f; level += 5.0f) {
                EXPECT_FLOAT_EQ(computer.gain_db(level), 0.0f);
            }
        }
    }
}

TEST(GainComputer, RangeCapsTheGainChange) {
    for (const DynamicsMode mode : k_modes) {
        GainComputer computer = hard(mode, 10.0f);
        computer.m_range_db = 6.0f;
        for (float level = -100.0f; level <= 20.0f; level += 1.0f) {
            EXPECT_LE(std::abs(computer.gain_db(level)), 6.0f);
        }
        EXPECT_FLOAT_EQ(std::abs(computer.gain_db(-100.0f)) + std::abs(computer.gain_db(20.0f)),
                        6.0f);
    }
}

TEST(GainComputer, SoftKneeIsContinuousWithItsSlopeAndMeetsTheHardCurve) {
    for (const DynamicsMode mode : k_modes) {
        GainComputer soft = hard(mode, 4.0f);
        soft.m_knee_db = 12.0f;
        const GainComputer reference = hard(mode, 4.0f);
        // Outside the knee: the hard curve.
        for (const float level : {-60.0f, -27.0f, -13.0f, 0.0f}) {
            EXPECT_FLOAT_EQ(soft.gain_db(level), reference.gain_db(level)) << level;
        }
        // At the threshold: slope · W / 8 off the hard curve's corner.
        const bool compression =
            mode == DynamicsMode::DownwardCompression || mode == DynamicsMode::UpwardCompression;
        const float slope = compression ? 0.75f : 3.0f;
        EXPECT_NEAR(std::abs(soft.gain_db(-20.0f)), slope * 12.0f / 8.0f, 1e-5f);
        // Continuous and with a continuous slope: second differences stay small.
        constexpr float k_step = 0.01f;
        for (float level = -40.0f; level < 0.0f; level += k_step) {
            const float a = soft.gain_db(level - k_step);
            const float b = soft.gain_db(level);
            const float c = soft.gain_db(level + k_step);
            ASSERT_NEAR(b - a, c - b, 1e-3f) << level;
        }
    }
}

TEST(GainComputer, DownwardModesLowerAndUpwardModesRaise) {
    GainComputer computer;
    computer.m_knee_db = 6.0f;
    for (float level = -100.0f; level <= 20.0f; level += 0.5f) {
        computer.m_mode = DynamicsMode::DownwardCompression;
        EXPECT_LE(computer.gain_db(level), 0.0f);
        computer.m_mode = DynamicsMode::DownwardExpansion;
        EXPECT_LE(computer.gain_db(level), 0.0f);
        computer.m_mode = DynamicsMode::UpwardCompression;
        EXPECT_GE(computer.gain_db(level), 0.0f);
        computer.m_mode = DynamicsMode::UpwardExpansion;
        EXPECT_GE(computer.gain_db(level), 0.0f);
    }
}
