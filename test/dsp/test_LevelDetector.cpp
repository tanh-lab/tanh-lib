#include <gtest/gtest.h>
#include <tanh/dsp/dynamics/LevelDetector.h>
#include <tanh/dsp/dynamics/StereoLink.h>
#include <tanh/dsp/utils/Decibels.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>

using thl::dsp::dynamics::DetectorMode;
using thl::dsp::dynamics::LevelDetector;
using thl::dsp::dynamics::link_powers;
using thl::dsp::dynamics::StereoLink;
using thl::dsp::utils::power_to_db;

namespace {
constexpr double k_sample_rate = 48000.0;
}  // namespace

TEST(LevelDetector, PeakIsTheSquare) {
    LevelDetector detector;
    detector.prepare(k_sample_rate);
    EXPECT_FLOAT_EQ(detector.process(0.5f), 0.25f);
    EXPECT_FLOAT_EQ(detector.process(-0.5f), 0.25f);
    EXPECT_NEAR(power_to_db(detector.process(0.5f)), -6.0206f, 1e-4f);
}

TEST(LevelDetector, RmsOfASineIsThreeDecibelsUnderItsPeak) {
    LevelDetector detector;
    detector.prepare(k_sample_rate);
    detector.set_mode(DetectorMode::Rms);
    detector.set_rms_window_ms(100.0f);
    float power = 0.0f;
    float lowest = 1.0f;
    float highest = 0.0f;
    for (int n = 0; n < 96000; ++n) {
        const auto x = static_cast<float>(
            std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(n) / k_sample_rate));
        power = detector.process(x);
        if (n > 48000) {
            lowest = std::min(lowest, power);
            highest = std::max(highest, power);
        }
    }
    EXPECT_NEAR(power_to_db(power), -3.0103f, 0.01f);
    EXPECT_LT(power_to_db(highest) - power_to_db(lowest), 0.02f);  // little ripple
}

TEST(LevelDetector, RmsWindowIsATimeConstant) {
    LevelDetector detector;
    detector.prepare(k_sample_rate);
    detector.set_mode(DetectorMode::Rms);
    detector.set_rms_window_ms(10.0f);
    float power = 0.0f;
    for (int n = 0; n < 480; ++n) { power = detector.process(1.0f); }
    EXPECT_NEAR(power, 1.0f - std::exp(-1.0f), 1e-3f);
    detector.reset();
    EXPECT_FLOAT_EQ(detector.process(0.0f), 0.0f);
}

TEST(LevelDetector, NonFiniteSamplesReadAsSilence) {
    for (const float bad :
         {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
        LevelDetector detector;
        LevelDetector reference;
        for (LevelDetector* d : {&detector, &reference}) {
            d->prepare(k_sample_rate);
            d->set_mode(DetectorMode::Rms);
            for (int n = 0; n < 480; ++n) { (void)d->process(1.0f); }
        }
        EXPECT_EQ(detector.process(bad), reference.process(0.0f));
        EXPECT_EQ(detector.process(1.0f), reference.process(1.0f));  // and goes on
    }
}

TEST(LevelDetector, AModeChangeClearsTheRmsWindow) {
    LevelDetector detector;
    detector.prepare(k_sample_rate);
    detector.set_mode(DetectorMode::Rms);
    for (int n = 0; n < 4800; ++n) { (void)detector.process(1.0f); }
    detector.set_mode(DetectorMode::Peak);
    detector.set_mode(DetectorMode::Rms);
    EXPECT_LT(detector.process(0.0f), 1e-6f);
    detector.set_mode(DetectorMode::Rms);  // unchanged: nothing cleared
    EXPECT_LT(detector.process(1.0f), 0.01f);
    EXPECT_GT(detector.process(1.0f), 0.0f);
}

TEST(LevelDetector, LinkedTakesTheLoudestAverageTheMeanPower) {
    const std::array<float, 2> powers{1.0f, 0.0f};
    EXPECT_FLOAT_EQ(link_powers(StereoLink::Linked, powers), 1.0f);
    EXPECT_NEAR(power_to_db(link_powers(StereoLink::Average, powers)), -3.0103f, 1e-4f);
}
