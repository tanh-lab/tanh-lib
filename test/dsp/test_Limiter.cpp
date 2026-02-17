#include <gtest/gtest.h>
#include <tanh/dsp/utils/Limiter.h>

#include <cmath>
#include <vector>

using namespace thl::dsp::utils;

// Concrete test subclass providing parameter values directly
class TestLimiter : public LimiterImpl {
public:
    float threshold_db = -3.0f;   // ~0.7079 linear
    float attack_ms = 0.5f;
    float release_ms = 100.0f;

    float threshold_linear() const {
        return std::pow(10.0f, threshold_db / 20.0f);
    }

private:
    float get_parameter_float(Parameter p) override {
        switch (p) {
            case Threshold: return threshold_db;
            case Attack: return attack_ms;
            case Release: return release_ms;
            default: return 0.0f;
        }
    }
    bool get_parameter_bool(Parameter /*p*/) override { return false; }
    int get_parameter_int(Parameter /*p*/) override { return 0; }
};

static constexpr double kSampleRate = 48000.0;
static constexpr size_t kBlockSize = 512;

// Helper: process a mono buffer of N identical samples, return the last output
static float process_sustained(TestLimiter& limiter, float value, size_t num_samples) {
    std::vector<float> buf(num_samples, value);
    float* ptrs[1] = { buf.data() };
    limiter.process(ptrs, num_samples, 1);
    return buf[num_samples - 1];
}

// =============================================================================
// Pass-through below threshold
// =============================================================================

TEST(Limiter, PassThroughBelowThreshold) {
    TestLimiter limiter;
    limiter.prepare(kSampleRate, kBlockSize, 1);

    // Signals well below threshold pass through unchanged
    EXPECT_FLOAT_EQ(process_sustained(limiter, 0.0f, 512), 0.0f);
    EXPECT_NEAR(process_sustained(limiter, 0.3f, 512), 0.3f, 0.001f);
    EXPECT_NEAR(process_sustained(limiter, -0.5f, 512), -0.5f, 0.001f);
}

// =============================================================================
// Gain reduction above threshold
// =============================================================================

TEST(Limiter, ReducesAboveThreshold) {
    TestLimiter limiter;
    limiter.prepare(kSampleRate, kBlockSize, 1);

    // Sustained signal above threshold should be reduced
    float output = process_sustained(limiter, 1.0f, 4800);  // 100ms of signal
    float thresh = limiter.threshold_linear();
    EXPECT_LE(std::fabs(output), thresh + 0.01f);
}

TEST(Limiter, ReducesNegativeAboveThreshold) {
    TestLimiter limiter;
    limiter.prepare(kSampleRate, kBlockSize, 1);

    float output = process_sustained(limiter, -1.0f, 4800);
    float thresh = limiter.threshold_linear();
    EXPECT_LE(std::fabs(output), thresh + 0.01f);
}

// =============================================================================
// Brickwall: output converges to threshold for loud signals
// =============================================================================

TEST(Limiter, OutputConvergesToThreshold) {
    TestLimiter limiter;
    limiter.threshold_db = -6.0f;  // 0.5012 linear
    limiter.attack_ms = 0.1f;      // very fast attack
    limiter.prepare(kSampleRate, kBlockSize, 1);

    // Sustained loud signal — after enough samples, output ≈ threshold
    float output = process_sustained(limiter, 2.0f, 9600);  // 200ms
    float thresh = limiter.threshold_linear();
    EXPECT_NEAR(std::fabs(output), thresh, 0.02f);
}

TEST(Limiter, LouderInputSameOutput) {
    // Louder input should converge to same threshold ceiling
    TestLimiter limiter;
    limiter.threshold_db = -6.0f;
    limiter.attack_ms = 0.1f;
    limiter.prepare(kSampleRate, kBlockSize, 1);
    float out_loud = process_sustained(limiter, 5.0f, 9600);

    limiter.prepare(kSampleRate, kBlockSize, 1);  // reset
    float out_medium = process_sustained(limiter, 1.0f, 9600);

    float thresh = limiter.threshold_linear();
    EXPECT_NEAR(std::fabs(out_loud), thresh, 0.02f);
    EXPECT_NEAR(std::fabs(out_medium), thresh, 0.02f);
}

// =============================================================================
// Fast attack catches transients
// =============================================================================

TEST(Limiter, FastAttackCatchesTransient) {
    TestLimiter limiter;
    limiter.threshold_db = -6.0f;
    limiter.attack_ms = 0.5f;
    limiter.prepare(kSampleRate, kBlockSize, 1);

    // Process a burst of loud samples (0.5ms = ~24 samples at 48kHz)
    size_t attack_samples = static_cast<size_t>(0.5 * 0.001 * kSampleRate) * 4;  // 4x attack time
    float output = process_sustained(limiter, 2.0f, attack_samples);
    float thresh = limiter.threshold_linear();

    // After 4x attack time, should be close to threshold
    EXPECT_LE(std::fabs(output), thresh + 0.05f);
}

// =============================================================================
// Release: gain recovers after loud signal stops
// =============================================================================

TEST(Limiter, GainRecoversAfterRelease) {
    TestLimiter limiter;
    limiter.threshold_db = -6.0f;
    limiter.attack_ms = 0.1f;
    limiter.release_ms = 50.0f;
    limiter.prepare(kSampleRate, kBlockSize, 1);

    // Drive limiter with loud signal
    process_sustained(limiter, 2.0f, 4800);

    // Now feed quiet signal for longer than release time
    float output = process_sustained(limiter, 0.3f, 24000);  // 500ms (10x release)

    // Gain should have recovered — quiet signal passes through
    EXPECT_NEAR(output, 0.3f, 0.01f);
}

// =============================================================================
// Stereo: peak detection across channels, gain applied equally
// =============================================================================

TEST(Limiter, StereoLinking) {
    TestLimiter limiter;
    limiter.threshold_db = -6.0f;
    limiter.attack_ms = 0.1f;
    limiter.prepare(kSampleRate, kBlockSize, 2);

    constexpr size_t N = 4800;
    std::vector<float> left(N, 0.1f);   // quiet
    std::vector<float> right(N, 2.0f);  // loud
    float* ptrs[2] = { left.data(), right.data() };
    limiter.process(ptrs, N, 2);

    // Right channel peaks trigger gain reduction on BOTH channels
    float thresh = limiter.threshold_linear();
    EXPECT_LE(std::fabs(right[N - 1]), thresh + 0.02f);
    // Left channel also reduced (stereo linked)
    EXPECT_LT(std::fabs(left[N - 1]), 0.1f);
}

// =============================================================================
// Different thresholds
// =============================================================================

TEST(Limiter, ThresholdChangesLimit) {
    // Higher threshold = less limiting
    TestLimiter limiter_high;
    limiter_high.threshold_db = -1.0f;  // 0.89
    limiter_high.attack_ms = 0.1f;
    limiter_high.prepare(kSampleRate, kBlockSize, 1);
    float out_high = process_sustained(limiter_high, 2.0f, 9600);

    TestLimiter limiter_low;
    limiter_low.threshold_db = -12.0f;  // 0.25
    limiter_low.attack_ms = 0.1f;
    limiter_low.prepare(kSampleRate, kBlockSize, 1);
    float out_low = process_sustained(limiter_low, 2.0f, 9600);

    EXPECT_GT(std::fabs(out_high), std::fabs(out_low));
}

TEST(Limiter, ZeroDbThresholdNoLimiting) {
    TestLimiter limiter;
    limiter.threshold_db = 0.0f;  // 1.0 linear
    limiter.prepare(kSampleRate, kBlockSize, 1);

    // Signal at 0.8 is below 1.0 threshold — passes through
    float output = process_sustained(limiter, 0.8f, 512);
    EXPECT_NEAR(output, 0.8f, 0.001f);
}
