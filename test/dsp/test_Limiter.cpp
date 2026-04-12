#include <gtest/gtest.h>
#include <tanh/dsp/audio/AudioBufferView.h>
#include <tanh/dsp/utils/Limiter.h>

#include <array>
#include <cmath>
#include <vector>

using namespace thl::dsp::utils;

// Concrete test subclass providing parameter values directly
class TestLimiter : public LimiterImpl {
public:
    float m_threshold_db = -3.0f;  // ~0.7079 linear
    float m_attack_ms = 0.5f;
    float m_release_ms = 100.0f;

    float threshold_linear() const { return std::pow(10.0f, m_threshold_db / 20.0f); }

private:
    float get_parameter_float(Parameter p, uint32_t /*modulation_offset*/) override {
        switch (p) {
            case Threshold: return m_threshold_db;
            case Attack: return m_attack_ms;
            case Release: return m_release_ms;
            default: return 0.0f;
        }
    }
};

static constexpr double k_sample_rate = 48000.0;
static constexpr size_t k_block_size = 512;

// Helper: process a mono buffer of N identical samples, return the last output
static float process_sustained(TestLimiter& limiter, float value, size_t num_samples) {
    std::vector<float> buf(num_samples, value);
    thl::dsp::audio::AudioBufferView view(buf.data(), num_samples);
    limiter.process(view);
    return buf[num_samples - 1];
}

// =============================================================================
// Pass-through below threshold
// =============================================================================

TEST(Limiter, PassThroughBelowThreshold) {
    TestLimiter limiter;
    limiter.prepare(k_sample_rate, k_block_size, 1);

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
    limiter.prepare(k_sample_rate, k_block_size, 1);

    // Sustained signal above threshold should be reduced
    float output = process_sustained(limiter, 1.0f, 4800);  // 100ms of signal
    float thresh = limiter.threshold_linear();
    EXPECT_LE(std::fabs(output), thresh + 0.01f);
}

TEST(Limiter, ReducesNegativeAboveThreshold) {
    TestLimiter limiter;
    limiter.prepare(k_sample_rate, k_block_size, 1);

    float output = process_sustained(limiter, -1.0f, 4800);
    float thresh = limiter.threshold_linear();
    EXPECT_LE(std::fabs(output), thresh + 0.01f);
}

// =============================================================================
// Brickwall: output converges to threshold for loud signals
// =============================================================================

TEST(Limiter, OutputConvergesToThreshold) {
    TestLimiter limiter;
    limiter.m_threshold_db = -6.0f;  // 0.5012 linear
    limiter.m_attack_ms = 0.1f;      // very fast attack
    limiter.prepare(k_sample_rate, k_block_size, 1);

    // Sustained loud signal — after enough samples, output ≈ threshold
    float output = process_sustained(limiter, 2.0f, 9600);  // 200ms
    float thresh = limiter.threshold_linear();
    EXPECT_NEAR(std::fabs(output), thresh, 0.02f);
}

TEST(Limiter, LouderInputSameOutput) {
    // Louder input should converge to same threshold ceiling
    TestLimiter limiter;
    limiter.m_threshold_db = -6.0f;
    limiter.m_attack_ms = 0.1f;
    limiter.prepare(k_sample_rate, k_block_size, 1);
    float out_loud = process_sustained(limiter, 5.0f, 9600);

    limiter.prepare(k_sample_rate, k_block_size, 1);  // reset
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
    limiter.m_threshold_db = -6.0f;
    limiter.m_attack_ms = 0.5f;
    limiter.prepare(k_sample_rate, k_block_size, 1);

    // Process a burst of loud samples (0.5ms = ~24 samples at 48kHz)
    size_t attack_samples = static_cast<size_t>(0.5 * 0.001 * k_sample_rate) * 4;  // 4x attack
                                                                                   // time
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
    limiter.m_threshold_db = -6.0f;
    limiter.m_attack_ms = 0.1f;
    limiter.m_release_ms = 50.0f;
    limiter.prepare(k_sample_rate, k_block_size, 1);

    // Drive limiter with loud signal
    process_sustained(limiter, 2.0f, 4800);

    // Now feed quiet signal for longer than release time
    float output = process_sustained(limiter, 0.3f, 24000);  // 500ms (10x
                                                             // release)

    // Gain should have recovered — quiet signal passes through
    EXPECT_NEAR(output, 0.3f, 0.01f);
}

// =============================================================================
// Stereo: peak detection across channels, gain applied equally
// =============================================================================

TEST(Limiter, StereoLinking) {
    TestLimiter limiter;
    limiter.m_threshold_db = -6.0f;
    limiter.m_attack_ms = 0.1f;
    limiter.prepare(k_sample_rate, k_block_size, 2);

    constexpr size_t k_n = 4800;
    std::vector<float> left(k_n, 0.1f);   // quiet
    std::vector<float> right(k_n, 2.0f);  // loud
    std::array<float*, 2> ptrs = {left.data(), right.data()};
    thl::dsp::audio::AudioBufferView stereo_view(ptrs.data(), 2, k_n);
    limiter.process(stereo_view);

    // Right channel peaks trigger gain reduction on BOTH channels
    float thresh = limiter.threshold_linear();
    EXPECT_LE(std::fabs(right[k_n - 1]), thresh + 0.02f);
    // Left channel also reduced (stereo linked)
    EXPECT_LT(std::fabs(left[k_n - 1]), 0.1f);
}

// =============================================================================
// Different thresholds
// =============================================================================

TEST(Limiter, ThresholdChangesLimit) {
    // Higher threshold = less limiting
    TestLimiter limiter_high;
    limiter_high.m_threshold_db = -1.0f;  // 0.89
    limiter_high.m_attack_ms = 0.1f;
    limiter_high.prepare(k_sample_rate, k_block_size, 1);
    float out_high = process_sustained(limiter_high, 2.0f, 9600);

    TestLimiter limiter_low;
    limiter_low.m_threshold_db = -12.0f;  // 0.25
    limiter_low.m_attack_ms = 0.1f;
    limiter_low.prepare(k_sample_rate, k_block_size, 1);
    float out_low = process_sustained(limiter_low, 2.0f, 9600);

    EXPECT_GT(std::fabs(out_high), std::fabs(out_low));
}

TEST(Limiter, ZeroDbThresholdNoLimiting) {
    TestLimiter limiter;
    limiter.m_threshold_db = 0.0f;  // 1.0 linear
    limiter.prepare(k_sample_rate, k_block_size, 1);

    // Signal at 0.8 is below 1.0 threshold — passes through
    float output = process_sustained(limiter, 0.8f, 512);
    EXPECT_NEAR(output, 0.8f, 0.001f);
}
