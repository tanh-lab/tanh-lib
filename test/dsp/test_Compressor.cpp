#include <gtest/gtest.h>
#include <tanh/core/Buffer.h>
#include <tanh/core/BufferView.h>
#include <tanh/dsp/dynamics/Compressor.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>

using thl::core::Buffer;
using thl::core::BufferView;
using thl::dsp::dynamics::CompressorImpl;
using thl::dsp::dynamics::DetectorMode;
using thl::dsp::dynamics::DynamicsMode;
using thl::dsp::dynamics::StereoLink;

namespace {

constexpr double k_sample_rate = 48000.0;
constexpr size_t k_block = 64;

class TestCompressor : public CompressorImpl {
public:
    TestCompressor() {
        m_values.fill(std::numeric_limits<float>::quiet_NaN());  // the defaults
        set(Knee, 0.0f);
        set(Attack, 0.0f);
        set(Release, 0.0f);
    }

    using CompressorImpl::Attack;
    using CompressorImpl::AutoMakeup;
    using CompressorImpl::AutoRelease;
    using CompressorImpl::Detector;
    using CompressorImpl::Hold;
    using CompressorImpl::Knee;
    using CompressorImpl::Link;
    using CompressorImpl::Lookahead;
    using CompressorImpl::Makeup;
    using CompressorImpl::Mix;
    using CompressorImpl::Mode;
    using CompressorImpl::Parameter;
    using CompressorImpl::Range;
    using CompressorImpl::Ratio;
    using CompressorImpl::Release;
    using CompressorImpl::RmsWindow;
    using CompressorImpl::SidechainHighPass;
    using CompressorImpl::Threshold;
    void set(Parameter parameter, float value) { m_values[parameter] = value; }

    std::array<float, NumParameters> m_values{};
    // The threshold's reads' offsets; fixed, as process() runs under RTSan.
    std::array<uint32_t, 64> m_offsets{};
    size_t m_num_offsets = 0;

protected:
    float get_parameter_float(Parameter parameter, uint32_t modulation_offset) override {
        if (parameter == Threshold && m_num_offsets < m_offsets.size()) {
            m_offsets[m_num_offsets++] = modulation_offset;
        }
        return m_values[parameter];
    }
};

using P = TestCompressor;

float db(double amplitude) {
    return static_cast<float>(20.0 * std::log10(std::abs(amplitude)));
}
float amplitude(float level_db) {
    return std::pow(10.0f, level_db / 20.0f);
}

Buffer<float> constant(size_t channels, size_t num_samples, float value) {
    Buffer<float> signal(channels, num_samples);
    for (size_t ch = 0; ch < channels; ++ch) {
        std::fill_n(signal.get_write_pointer(ch), num_samples, value);
    }
    return signal;
}

void run(TestCompressor& compressor, Buffer<float>& signal, Buffer<float>* sidechain = nullptr) {
    const size_t num_samples = signal.get_num_samples();
    for (size_t start = 0; start < num_samples; start += k_block) {
        const size_t length = std::min(k_block, num_samples - start);
        if (sidechain != nullptr) {
            compressor.process_with_sidechain(BufferView(signal).sub_block(start, length),
                                              BufferView(*sidechain).sub_block(start, length));
        } else {
            compressor.process(BufferView(signal).sub_block(start, length));
        }
    }
}

/// The level of a constant input after the compressor, at its end.
float steady_level_db(TestCompressor& compressor, float input_db) {
    compressor.prepare(k_sample_rate, k_block, 2);
    Buffer<float> signal = constant(2, 4800, amplitude(input_db));
    run(compressor, signal);
    return db(signal.get_read_pointer(0)[4799]);
}

}  // namespace

TEST(Compressor, DownwardCompressesAboveTheThreshold) {
    TestCompressor compressor;
    compressor.set(P::Threshold, -18.0f);
    compressor.set(P::Ratio, 4.0f);
    EXPECT_NEAR(steady_level_db(compressor, -6.0f), -15.0f, 1e-3f);  // 12 over: 3 over
    EXPECT_NEAR(steady_level_db(compressor, -30.0f), -30.0f, 1e-3f);
}

TEST(Compressor, UpwardCompressionRaisesQuietSignals) {
    TestCompressor compressor;
    compressor.set(P::Mode, static_cast<float>(DynamicsMode::UpwardCompression));
    compressor.set(P::Threshold, -20.0f);
    compressor.set(P::Ratio, 2.0f);
    EXPECT_NEAR(steady_level_db(compressor, -40.0f), -30.0f, 1e-3f);
    EXPECT_NEAR(steady_level_db(compressor, -10.0f), -10.0f, 1e-3f);
}

TEST(Compressor, DownwardExpansionGatesWithItsRange) {
    TestCompressor compressor;
    compressor.set(P::Mode, static_cast<float>(DynamicsMode::DownwardExpansion));
    compressor.set(P::Threshold, -30.0f);
    compressor.set(P::Ratio, std::numeric_limits<float>::infinity());
    compressor.set(P::Range, 40.0f);
    EXPECT_NEAR(steady_level_db(compressor, -50.0f), -90.0f, 1e-3f);
    EXPECT_NEAR(steady_level_db(compressor, -20.0f), -20.0f, 1e-3f);
}

TEST(Compressor, UpwardExpansionRaisesLoudSignals) {
    TestCompressor compressor;
    compressor.set(P::Mode, static_cast<float>(DynamicsMode::UpwardExpansion));
    compressor.set(P::Threshold, -20.0f);
    compressor.set(P::Ratio, 1.5f);
    EXPECT_NEAR(steady_level_db(compressor, -10.0f), -5.0f, 1e-3f);
}

TEST(Compressor, RmsDetectorReadsASineThreeDecibelsUnderItsPeak) {
    TestCompressor compressor;
    compressor.set(P::Threshold, -40.0f);
    compressor.set(P::Ratio, std::numeric_limits<float>::infinity());
    compressor.set(P::Detector, static_cast<float>(DetectorMode::Rms));
    compressor.set(P::RmsWindow, 50.0f);
    compressor.set(P::Release, 50.0f);
    compressor.set(P::Attack, 50.0f);
    compressor.prepare(k_sample_rate, k_block, 1);
    Buffer<float> sine(1, 96000);
    for (size_t n = 0; n < sine.get_num_samples(); ++n) {
        sine.get_write_pointer(0)[n] =
            static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * 1000.0 *
                                              static_cast<double>(n) / k_sample_rate));
    }
    run(compressor, sine);
    // Limited to an RMS of -40 dB: the peak 3.01 dB above it.
    float peak = 0.0f;
    for (size_t n = 48000; n < 96000; ++n) {
        peak = std::max(peak, std::abs(sine.get_read_pointer(0)[n]));
    }
    EXPECT_NEAR(db(peak), -40.0f + 3.0103f, 0.05f);
}

TEST(Compressor, LookaheadIsTheLatencyAndTheGainLeadsTheAudio) {
    TestCompressor compressor;
    compressor.set(P::Threshold, -20.0f);
    compressor.set(P::Ratio, std::numeric_limits<float>::infinity());
    compressor.set(P::Lookahead, 5.0f);
    compressor.set(P::Attack, 0.5f);
    compressor.set(P::Release, 100.0f);
    compressor.prepare(k_sample_rate, k_block, 1);
    EXPECT_EQ(compressor.latency_samples(), 240u);
    EXPECT_EQ(compressor.max_latency_samples(), 960u);
    // Silence, then a step to 0 dBFS: it comes out 240 samples later, already limited.
    Buffer<float> step(1, 4800);
    std::fill_n(step.get_write_pointer(0) + 1000, 3800, 1.0f);
    run(compressor, step);
    EXPECT_EQ(step.get_read_pointer(0)[1239], 0.0f);
    EXPECT_GT(step.get_read_pointer(0)[1240], 0.0f);
    EXPECT_LT(db(step.get_read_pointer(0)[1240]), -19.0f);  // 10 attack times already passed
    // Without lookahead the step's first sample passes at nearly its full level.
    TestCompressor direct;
    direct.m_values = compressor.m_values;
    direct.set(P::Lookahead, 0.0f);
    direct.prepare(k_sample_rate, k_block, 1);
    EXPECT_EQ(direct.latency_samples(), 0u);
    Buffer<float> step2(1, 4800);
    std::fill_n(step2.get_write_pointer(0) + 1000, 3800, 1.0f);
    run(direct, step2);
    EXPECT_GT(db(step2.get_read_pointer(0)[1000]), -1.0f);
}

TEST(Compressor, LinkedChannelsShareTheGainDualMonoDoesNot) {
    const auto quiet_channel_db = [](StereoLink link) {
        TestCompressor compressor;
        compressor.set(P::Threshold, -20.0f);
        compressor.set(P::Ratio, std::numeric_limits<float>::infinity());
        compressor.set(P::Link, static_cast<float>(link));
        compressor.prepare(k_sample_rate, k_block, 2);
        Buffer<float> signal(2, 4800);
        std::fill_n(signal.get_write_pointer(0), 4800, 1.0f);               // 0 dBFS
        std::fill_n(signal.get_write_pointer(1), 4800, amplitude(-30.0f));  // under the threshold
        run(compressor, signal);
        return db(signal.get_read_pointer(1)[4799]);
    };
    EXPECT_NEAR(quiet_channel_db(StereoLink::Linked), -50.0f, 1e-3f);
    EXPECT_NEAR(quiet_channel_db(StereoLink::DualMono), -30.0f, 1e-3f);
    // Average: the mean power of 0 dB and -30 dB is -3.01 dB, 16.99 over.
    EXPECT_NEAR(quiet_channel_db(StereoLink::Average), -30.0f - 16.99f, 0.01f);
}

TEST(Compressor, AnExternalSidechainKeysTheGain) {
    TestCompressor compressor;
    compressor.set(P::Threshold, -20.0f);
    compressor.set(P::Ratio, std::numeric_limits<float>::infinity());
    compressor.prepare(k_sample_rate, k_block, 2);
    Buffer<float> signal = constant(2, 4800, amplitude(-30.0f));
    Buffer<float> key = constant(1, 4800, amplitude(-6.0f));  // one channel keys both
    run(compressor, signal, &key);
    EXPECT_NEAR(db(signal.get_read_pointer(0)[4799]), -44.0f, 1e-3f);
    EXPECT_NEAR(db(signal.get_read_pointer(1)[4799]), -44.0f, 1e-3f);
}

TEST(Compressor, SidechainHighPassIgnoresTheBass) {
    const auto reduction_db = [](float high_pass_hz) {
        TestCompressor compressor;
        compressor.set(P::Threshold, -20.0f);
        compressor.set(P::Ratio, 10.0f);
        compressor.set(P::Attack, 1.0f);
        compressor.set(P::Release, 100.0f);
        compressor.set(P::SidechainHighPass, high_pass_hz);
        compressor.prepare(k_sample_rate, k_block, 1);
        Buffer<float> bass(1, 48000);
        for (size_t n = 0; n < bass.get_num_samples(); ++n) {
            bass.get_write_pointer(0)[n] =
                static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * 40.0 *
                                                  static_cast<double>(n) / k_sample_rate));
        }
        run(compressor, bass);
        return compressor.gain_change_db();
    };
    EXPECT_LT(reduction_db(0.0f), -10.0f);
    EXPECT_GT(reduction_db(1000.0f), -0.01f);  // 40 Hz is 26 dB under the threshold through it
}

TEST(Compressor, MixBlendsTheDelayedDrySignal) {
    for (const float mix : {0.0f, 0.5f}) {
        TestCompressor compressor;
        compressor.set(P::Threshold, -20.0f);
        compressor.set(P::Ratio, std::numeric_limits<float>::infinity());
        compressor.set(P::Lookahead, 1.0f);
        compressor.set(P::Mix, mix);
        compressor.prepare(k_sample_rate, k_block, 1);
        Buffer<float> signal = constant(1, 4800, 1.0f);
        run(compressor, signal);
        const float wet = amplitude(-20.0f);
        EXPECT_NEAR(signal.get_read_pointer(0)[4799], (1.0f - mix) + (mix * wet), 1e-5f) << mix;
        EXPECT_EQ(signal.get_read_pointer(0)[47], 0.0f);  // the dry part delayed too
    }
}

TEST(Compressor, MakeupAndAutoMakeup) {
    TestCompressor compressor;
    compressor.set(P::Threshold, -18.0f);
    compressor.set(P::Ratio, 4.0f);
    compressor.set(P::Makeup, 2.0f);
    EXPECT_NEAR(steady_level_db(compressor, -30.0f), -28.0f, 1e-3f);
    // The curve at 0 dBFS: -13.5 dB; auto makeup: +6.75 dB.
    compressor.set(P::AutoMakeup, 1.0f);
    EXPECT_NEAR(steady_level_db(compressor, -30.0f), -30.0f + 2.0f + 6.75f, 1e-3f);
}

TEST(Compressor, AttackRampsTheGainInsteadOfJumping) {
    TestCompressor compressor;
    compressor.set(P::Threshold, -20.0f);
    compressor.set(P::Ratio, std::numeric_limits<float>::infinity());
    compressor.set(P::Attack, 10.0f);
    compressor.prepare(k_sample_rate, k_block, 1);
    Buffer<float> signal = constant(1, 4800, 1.0f);
    run(compressor, signal);
    // After one attack time 63 % of the 20 dB.
    EXPECT_NEAR(db(signal.get_read_pointer(0)[479]), -20.0f * (1.0f - std::exp(-1.0f)), 0.05f);
    for (size_t n = 1; n < 4800; ++n) {
        ASSERT_LE(signal.get_read_pointer(0)[n], signal.get_read_pointer(0)[n - 1]) << n;
    }
}

TEST(Compressor, MetersHoldTheLastBlock) {
    TestCompressor compressor;
    compressor.set(P::Threshold, -20.0f);
    compressor.set(P::Ratio, std::numeric_limits<float>::infinity());
    EXPECT_NEAR(steady_level_db(compressor, -6.0f), -20.0f, 1e-3f);
    EXPECT_NEAR(compressor.gain_change_db(), -14.0f, 1e-3f);
    EXPECT_NEAR(compressor.input_peak_db(), -6.0f, 1e-3f);
    EXPECT_NEAR(compressor.output_peak_db(), -20.0f, 1e-3f);
}

TEST(Compressor, ResetClearsTheGainAndTheLookahead) {
    TestCompressor compressor;
    compressor.set(P::Threshold, -20.0f);
    compressor.set(P::Ratio, std::numeric_limits<float>::infinity());
    compressor.set(P::Release, 1000.0f);
    compressor.set(P::Lookahead, 2.0f);
    compressor.prepare(k_sample_rate, k_block, 1);
    Buffer<float> loud = constant(1, 4800, 1.0f);
    run(compressor, loud);
    compressor.reset();
    EXPECT_EQ(compressor.gain_change_db(), 0.0f);
    Buffer<float> quiet = constant(1, 480, amplitude(-30.0f));
    run(compressor, quiet);
    for (size_t n = 0; n < 96; ++n) { ASSERT_EQ(quiet.get_read_pointer(0)[n], 0.0f) << n; }
    EXPECT_NEAR(db(quiet.get_read_pointer(0)[479]), -30.0f, 1e-3f);  // no gain held over
}

TEST(Compressor, ParametersAreReadAtTheChangePoints) {
    TestCompressor compressor;
    compressor.prepare(k_sample_rate, k_block, 1);
    compressor.m_num_offsets = 0;
    Buffer<float> block(1, k_block);
    const std::array<uint32_t, 2> change_points{16, 40};
    compressor.process_modulated(BufferView(block), change_points);
    ASSERT_EQ(compressor.m_num_offsets, 3u);
    EXPECT_EQ(compressor.m_offsets[0], 0u);
    EXPECT_EQ(compressor.m_offsets[1], 16u);
    EXPECT_EQ(compressor.m_offsets[2], 40u);
}

TEST(Compressor, DryIsTheUndelayedInput) {
    // No phase shift: process_with_dry() leaves the latency to whoever mixes (a Graph).
    TestCompressor compressor;
    compressor.set(P::Lookahead, 1.0f);
    compressor.prepare(k_sample_rate, k_block, 1);
    Buffer<float> signal = constant(1, k_block, 0.25f);
    Buffer<float> dry(1, k_block);
    compressor.process_with_dry(BufferView(signal), 0, BufferView(dry));
    for (size_t n = 0; n < k_block; ++n) { ASSERT_EQ(dry.get_read_pointer(0)[n], 0.25f); }
}

TEST(Compressor, ReleaseAndHoldShapeTheRecovery) {
    TestCompressor compressor;
    compressor.set(P::Threshold, -20.0f);
    compressor.set(P::Ratio, std::numeric_limits<float>::infinity());
    compressor.set(P::Hold, 10.0f);
    compressor.set(P::Release, 50.0f);
    compressor.prepare(k_sample_rate, k_block, 1);
    Buffer<float> loud = constant(1, 4800, 1.0f);
    run(compressor, loud);
    Buffer<float> quiet = constant(1, 4800, amplitude(-40.0f));
    run(compressor, quiet);
    // Held for 10 ms at -20 dB, then one release time recovers 63 % of it.
    EXPECT_NEAR(db(quiet.get_read_pointer(0)[479]), -60.0f, 1e-3f);
    EXPECT_NEAR(db(quiet.get_read_pointer(0)[480 + 2400 - 1]),
                -40.0f - (20.0f * std::exp(-1.0f)),
                0.05f);
}
