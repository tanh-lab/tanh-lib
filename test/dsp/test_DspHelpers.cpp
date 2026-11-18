#include <gtest/gtest.h>
#include <tanh/dsp/utils/Decibels.h>
#include <tanh/dsp/utils/ParameterValue.h>
#include <tanh/dsp/utils/TimeConstant.h>

#include <cmath>
#include <cstdint>
#include <limits>

using thl::dsp::utils::db_to_gain;
using thl::dsp::utils::gain_to_db;
using thl::dsp::utils::k_min_db;
using thl::dsp::utils::ms_to_samples;
using thl::dsp::utils::one_pole_pole;
using thl::dsp::utils::ParameterRange;
using thl::dsp::utils::power_to_db;
using thl::dsp::utils::sanitised;
using thl::dsp::utils::to_enum;

namespace {
constexpr float k_nan = std::numeric_limits<float>::quiet_NaN();
constexpr float k_infinity = std::numeric_limits<float>::infinity();
enum class Choice : uint8_t { A, B, C };
}  // namespace

TEST(Decibels, ConvertBothWays) {
    EXPECT_FLOAT_EQ(db_to_gain(0.0f), 1.0f);
    EXPECT_NEAR(db_to_gain(-6.0206f), 0.5f, 1e-6f);
    EXPECT_NEAR(db_to_gain(20.0), 10.0, 1e-12);
    EXPECT_NEAR(gain_to_db(0.5f), -6.0206f, 1e-4f);
    EXPECT_NEAR(power_to_db(0.25f), -6.0206f, 1e-4f);
    for (const float db : {-90.0f, -12.5f, 0.0f, 6.0f, 24.0f}) {
        EXPECT_NEAR(gain_to_db(db_to_gain(db)), db, 1e-4f);
    }
}

TEST(Decibels, FloorSilenceAndNan) {
    EXPECT_EQ(gain_to_db(0.0f), k_min_db);
    EXPECT_EQ(power_to_db(0.0f), k_min_db);
    EXPECT_EQ(gain_to_db(1e-9f), k_min_db);
    EXPECT_EQ(gain_to_db(k_nan), k_min_db);
    EXPECT_EQ(gain_to_db(0.0, -240.0), -240.0);
}

TEST(TimeConstant, OnePolePole) {
    EXPECT_DOUBLE_EQ(ms_to_samples(10.0, 48000.0), 480.0);
    EXPECT_FLOAT_EQ(one_pole_pole(10.0, 48000.0), std::exp(-1.0f / 480.0f));
    EXPECT_FLOAT_EQ(one_pole_pole(0.0, 48000.0), 0.0f);
    EXPECT_FLOAT_EQ(one_pole_pole(-1.0), 0.0f);
}

TEST(ParameterValue, NanBecomesTheFallbackTheRestIsClamped) {
    EXPECT_EQ(sanitised(k_nan, 3.0f, 0.0f, 10.0f), 3.0f);
    EXPECT_EQ(sanitised(12.0f, 3.0f, 0.0f, 10.0f), 10.0f);
    EXPECT_EQ(sanitised(-k_infinity, 3.0f, 0.0f, 10.0f), 0.0f);
    EXPECT_EQ(sanitised(k_infinity, 3.0f, 1.0f, k_infinity), k_infinity);  // a ratio
    EXPECT_EQ(sanitised(k_nan, ParameterRange{.m_default = 4.0f, .m_min = 1.0f, .m_max = 8.0f}),
              4.0f);
}

TEST(ParameterValue, ToEnumRoundsAndClamps) {
    EXPECT_EQ(to_enum(1.4f, Choice::C, Choice::A), Choice::B);
    EXPECT_EQ(to_enum(1.6f, Choice::C, Choice::A), Choice::C);
    EXPECT_EQ(to_enum(9.0f, Choice::C, Choice::A), Choice::C);
    EXPECT_EQ(to_enum(-2.0f, Choice::C, Choice::B), Choice::A);
    EXPECT_EQ(to_enum(k_nan, Choice::C, Choice::B), Choice::B);
}
