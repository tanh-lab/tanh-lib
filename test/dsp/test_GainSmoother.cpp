#include <gtest/gtest.h>
#include <tanh/dsp/dynamics/GainSmoother.h>

#include <cmath>

using thl::dsp::dynamics::GainSmoother;

namespace {

constexpr double k_sample_rate = 48000.0;
constexpr int k_samples_per_ms = 48;

float run(GainSmoother& smoother, float target_db, int num_samples) {
    float gain = smoother.gain_db();
    for (int n = 0; n < num_samples; ++n) { gain = smoother.process(target_db); }
    return gain;
}

GainSmoother prepared(float attack_ms,
                      float release_ms,
                      float hold_ms = 0.0f,
                      bool automatic = false) {
    GainSmoother smoother;
    smoother.prepare(k_sample_rate);
    smoother.set_times(attack_ms, release_ms, hold_ms, automatic);
    return smoother;
}

}  // namespace

TEST(GainSmoother, AttackAndReleaseAreTimeConstants) {
    GainSmoother smoother = prepared(5.0f, 50.0f);
    const float one_minus_e = 1.0f - std::exp(-1.0f);
    EXPECT_NEAR(run(smoother, -10.0f, 5 * k_samples_per_ms), -10.0f * one_minus_e, 0.01f);
    smoother.reset(-10.0f);
    EXPECT_NEAR(run(smoother, 0.0f, 50 * k_samples_per_ms), -10.0f * (1.0f - one_minus_e), 0.01f);
}

TEST(GainSmoother, AttackMeansAwayFromZeroInBothDirections) {
    // Upward modes raise the gain: their attack moves it up, their release down.
    GainSmoother smoother = prepared(1.0f, 1000.0f);
    EXPECT_NEAR(run(smoother, 10.0f, 10 * k_samples_per_ms), 10.0f, 0.01f);
    EXPECT_GT(run(smoother, 0.0f, 10 * k_samples_per_ms), 9.0f);
}

TEST(GainSmoother, ZeroTimesJump) {
    GainSmoother smoother = prepared(0.0f, 0.0f);
    EXPECT_FLOAT_EQ(smoother.process(-12.0f), -12.0f);
    EXPECT_FLOAT_EQ(smoother.process(0.0f), 0.0f);
}

TEST(GainSmoother, HoldDelaysTheRelease) {
    GainSmoother smoother = prepared(0.0f, 10.0f, 5.0f);
    EXPECT_FLOAT_EQ(smoother.process(-6.0f), -6.0f);
    EXPECT_FLOAT_EQ(run(smoother, 0.0f, 5 * k_samples_per_ms), -6.0f);
    EXPECT_GT(smoother.process(0.0f), -6.0f);
}

TEST(GainSmoother, HoldRunsFromTheEndOfASustainedGainChange) {
    GainSmoother smoother = prepared(0.0f, 10.0f, 5.0f);
    run(smoother, -6.0f, 100 * k_samples_per_ms);
    EXPECT_FLOAT_EQ(run(smoother, 0.0f, 5 * k_samples_per_ms), -6.0f);
    EXPECT_GT(smoother.process(0.0f), -6.0f);
}

TEST(GainSmoother, AutoReleaseIsSlowAfterALongCompressionOnly) {
    const auto released_after = [](int compression_ms, bool automatic) {
        GainSmoother smoother = prepared(1.0f, 20.0f, 0.0f, automatic);
        run(smoother, -10.0f, compression_ms * k_samples_per_ms);
        return run(smoother, 0.0f, 100 * k_samples_per_ms);
    };
    // Without it, 100 ms after a 20 ms release almost all is back.
    EXPECT_GT(released_after(2000, false), -0.1f);
    // A short peak hardly moved the slow stage: most is back too.
    EXPECT_GT(released_after(2, true), -1.5f);
    // A long compression built it up: after 100 ms at its 200 ms release, most remains.
    EXPECT_LT(released_after(2000, true), -5.0f);
}

TEST(GainSmoother, ResetJumpsAndEndsTheHold) {
    GainSmoother smoother = prepared(10.0f, 10.0f, 100.0f);
    run(smoother, -20.0f, 4800);
    smoother.reset();
    EXPECT_FLOAT_EQ(smoother.gain_db(), 0.0f);
    EXPECT_FLOAT_EQ(smoother.process(0.0f), 0.0f);
}
