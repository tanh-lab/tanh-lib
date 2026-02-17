#include <gtest/gtest.h>
#include <tanh/dsp/utils/Limiter.h>

#include <cmath>

using namespace thl::dsp::utils;

// Concrete test subclass that provides parameter values directly (linear gain)
class TestLimiter : public LimiterImpl {
public:
    float threshold_gain = 0.7079f; // ~-3 dB

    void set_threshold(float gain) { threshold_gain = gain; }

private:
    float get_parameter_float(Parameter /*p*/) override { return threshold_gain; }
    bool get_parameter_bool(Parameter /*p*/) override { return false; }
    int get_parameter_int(Parameter /*p*/) override { return 0; }
};

// Helper: run limiter on a single sample via a 1-channel buffer
static float process_sample(TestLimiter& limiter, float input) {
    float buf = input;
    float* ptrs[1] = { &buf };
    limiter.process(ptrs, 1, 1);
    return buf;
}

// =============================================================================
// Pass-through below threshold
// =============================================================================

TEST(Limiter, PassThroughBelowThreshold) {
    TestLimiter limiter; // 0.7079

    EXPECT_FLOAT_EQ(process_sample(limiter, 0.0f), 0.0f);
    EXPECT_FLOAT_EQ(process_sample(limiter, 0.5f), 0.5f);
    EXPECT_FLOAT_EQ(process_sample(limiter, -0.5f), -0.5f);
    EXPECT_FLOAT_EQ(process_sample(limiter, 0.7f), 0.7f);
    EXPECT_FLOAT_EQ(process_sample(limiter, -0.7f), -0.7f);
}

// =============================================================================
// Soft clipping above threshold
// =============================================================================

TEST(Limiter, SoftClipsAboveThreshold) {
    TestLimiter limiter;

    float output = process_sample(limiter, 1.0f);
    EXPECT_GT(output, limiter.threshold_gain);
    EXPECT_LT(output, 1.0f);
}

TEST(Limiter, SoftClipsNegative) {
    TestLimiter limiter;

    float output = process_sample(limiter, -1.0f);
    EXPECT_LT(output, -limiter.threshold_gain);
    EXPECT_GT(output, -1.0f);
}

TEST(Limiter, SymmetricClipping) {
    TestLimiter limiter;
    float pos = process_sample(limiter, 2.0f);
    float neg = process_sample(limiter, -2.0f);
    EXPECT_FLOAT_EQ(pos, -neg);
}

TEST(Limiter, OutputNeverExceedsOne) {
    TestLimiter limiter;
    EXPECT_LE(process_sample(limiter, 10.0f), 1.0f);
    EXPECT_GE(process_sample(limiter, -10.0f), -1.0f);
    EXPECT_LE(process_sample(limiter, 100.0f), 1.0f);
    EXPECT_LT(process_sample(limiter, 1.5f), 1.0f);
}

TEST(Limiter, GradualCompression) {
    TestLimiter limiter;
    float out1 = process_sample(limiter, 0.8f);
    float out2 = process_sample(limiter, 1.0f);
    float out3 = process_sample(limiter, 2.0f);
    float out4 = process_sample(limiter, 4.0f);

    EXPECT_LT(out1, out2);
    EXPECT_LT(out2, out3);
    EXPECT_LT(out3, out4);

    float gain1 = out2 / 1.0f;
    float gain2 = out4 / 4.0f;
    EXPECT_LT(gain2, gain1);
}

// =============================================================================
// Multi-channel buffer processing
// =============================================================================

TEST(Limiter, ProcessStereoBuffer) {
    TestLimiter limiter;

    float left[]  = { 0.0f, 0.3f, -0.5f, 0.8f, -1.5f, 3.0f };
    float right[] = { 0.1f, -0.2f, 0.9f, -2.0f, 0.5f, 0.0f };
    float* ptrs[2] = { left, right };

    limiter.process(ptrs, 6, 2);

    // Below threshold: untouched
    EXPECT_FLOAT_EQ(left[0], 0.0f);
    EXPECT_FLOAT_EQ(left[1], 0.3f);
    EXPECT_FLOAT_EQ(left[2], -0.5f);
    // Above threshold: limited
    EXPECT_GT(left[3], limiter.threshold_gain);
    EXPECT_LT(left[3], 0.8f);
    EXPECT_LT(left[5], 1.0f);

    EXPECT_FLOAT_EQ(right[0], 0.1f);
    EXPECT_FLOAT_EQ(right[1], -0.2f);
    EXPECT_FLOAT_EQ(right[4], 0.5f);
    EXPECT_FLOAT_EQ(right[5], 0.0f);
    EXPECT_LT(right[2], 0.9f);
    EXPECT_GT(right[3], -2.0f);
}

// =============================================================================
// Different thresholds
// =============================================================================

TEST(Limiter, ThresholdChangesClipping) {
    TestLimiter limiter;

    // At 0.7079 (~-3 dB), 0.8 is clipped
    limiter.set_threshold(0.7079f);
    float out_low = process_sample(limiter, 0.8f);
    EXPECT_LT(out_low, 0.8f);

    // At 1.0 (0 dB), 0.8 passes through
    limiter.set_threshold(1.0f);
    float out_high = process_sample(limiter, 0.8f);
    EXPECT_FLOAT_EQ(out_high, 0.8f);
}

TEST(Limiter, ExactlyAtThreshold) {
    TestLimiter limiter;
    EXPECT_FLOAT_EQ(process_sample(limiter, limiter.threshold_gain), limiter.threshold_gain);
}
