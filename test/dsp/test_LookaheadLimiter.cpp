#include <gtest/gtest.h>
#include <tanh/core/Buffer.h>
#include <tanh/core/BufferView.h>
#include <tanh/dsp/dynamics/LevelDetector.h>
#include <tanh/dsp/dynamics/LookaheadLimiter.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <random>

using thl::core::Buffer;
using thl::core::BufferView;
using thl::dsp::dynamics::LookaheadLimiterImpl;
using thl::dsp::dynamics::StereoLink;

namespace {

constexpr double k_sample_rate = 48000.0;
constexpr size_t k_block = 64;

class TestLimiter : public LookaheadLimiterImpl {
public:
    TestLimiter() { m_values.fill(std::numeric_limits<float>::quiet_NaN()); }

    using LookaheadLimiterImpl::Ceiling;
    using LookaheadLimiterImpl::InputGain;
    using LookaheadLimiterImpl::Link;
    using LookaheadLimiterImpl::Lookahead;
    using LookaheadLimiterImpl::Parameter;
    using LookaheadLimiterImpl::Release;
    using LookaheadLimiterImpl::TruePeak;
    void set(Parameter parameter, float value) { m_values[parameter] = value; }

    std::array<float, NumParameters> m_values{};
    std::array<uint32_t, 64> m_offsets{};  // the ceiling's reads; fixed, as process() runs under
                                           // RTSan
    size_t m_num_offsets = 0;

protected:
    float get_parameter_float(Parameter parameter, uint32_t modulation_offset) override {
        if (parameter == Ceiling && m_num_offsets < m_offsets.size()) {
            m_offsets[m_num_offsets++] = modulation_offset;
        }
        return m_values[parameter];
    }
};

using P = TestLimiter;

float db(double amplitude) {
    return static_cast<float>(20.0 * std::log10(std::abs(amplitude)));
}
float amplitude(float level_db) {
    return std::pow(10.0f, level_db / 20.0f);
}

void run(TestLimiter& limiter, Buffer<float>& signal, size_t block = k_block) {
    const size_t num_samples = signal.get_num_samples();
    for (size_t start = 0; start < num_samples; start += block) {
        limiter.process(BufferView(signal).sub_block(start, std::min(block, num_samples - start)));
    }
}

Buffer<float> sine(size_t channels,
                   size_t num_samples,
                   double frequency,
                   float peak,
                   double phase = 0.0) {
    Buffer<float> signal(channels, num_samples);
    for (size_t ch = 0; ch < channels; ++ch) {
        for (size_t n = 0; n < num_samples; ++n) {
            signal.get_write_pointer(ch)[n] =
                static_cast<float>(peak * std::sin((2.0 * std::numbers::pi * frequency *
                                                    static_cast<double>(n) / k_sample_rate) +
                                                   phase));
        }
    }
    return signal;
}

/// Noise with loud bursts, peaks far above 0 dBFS.
Buffer<float> bursts(size_t channels, size_t num_samples, unsigned seed) {
    std::mt19937 generator(seed);
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);
    Buffer<float> signal(channels, num_samples);
    for (size_t ch = 0; ch < channels; ++ch) {
        for (size_t n = 0; n < num_samples; ++n) {
            const float level = ((n / 1500) % 3 == 0) ? 8.0f : 0.3f;
            signal.get_write_pointer(ch)[n] = level * noise(generator);
        }
    }
    return signal;
}

}  // namespace

TEST(LookaheadLimiter, BelowTheCeilingItIsADelay) {
    TestLimiter limiter;
    limiter.set(P::Lookahead, 2.0f);
    limiter.prepare(k_sample_rate, k_block, 2);
    EXPECT_EQ(limiter.latency_samples(), 95u);  // 96 - 1
    EXPECT_EQ(limiter.max_latency_samples(), 479u + LookaheadLimiterImpl::k_true_peak_delay);
    Buffer<float> signal = sine(2, 4800, 1000.0, 0.5f);
    const Buffer<float> input = signal;
    run(limiter, signal);
    for (size_t n = 95; n < 4800; ++n) {
        ASSERT_EQ(signal.get_read_pointer(0)[n], input.get_read_pointer(0)[n - 95]) << n;
    }
    for (size_t n = 0; n < 95; ++n) { ASSERT_EQ(signal.get_read_pointer(1)[n], 0.0f) << n; }
}

TEST(LookaheadLimiter, NoSampleAboveTheCeilingAndTheGainAloneGetsItThere) {
    for (const float lookahead : {0.5f, 2.0f, 5.0f}) {
        for (const float release : {0.0f, 20.0f, 200.0f}) {
            TestLimiter limiter;
            limiter.set(P::Ceiling, -1.0f);
            limiter.set(P::Lookahead, lookahead);
            limiter.set(P::Release, release);
            limiter.prepare(k_sample_rate, k_block, 2);
            Buffer<float> signal = bursts(2, 24000, 7);
            const Buffer<float> input = signal;
            run(limiter, signal);
            const size_t delay = limiter.latency_samples();
            const float ceiling = amplitude(-1.0f);
            for (size_t n = delay; n < 24000; ++n) {
                std::array<float, 2> gains{1.0f, 1.0f};
                for (size_t ch = 0; ch < 2; ++ch) {
                    const float y = signal.get_read_pointer(ch)[n];
                    ASSERT_LE(std::abs(y), ceiling) << n;
                    const float x = input.get_read_pointer(ch)[n - delay];
                    gains[ch] =
                        std::abs(x) > 1e-2f ? y / x : std::numeric_limits<float>::quiet_NaN();
                }
                // Linked: one gain for both channels, so no sample was clipped on its own.
                if (!std::isnan(gains[0]) && !std::isnan(gains[1])) {
                    ASSERT_NEAR(gains[0], gains[1], 1e-4f)
                        << n << " " << lookahead << " " << release;
                    ASSERT_LE(gains[0], 1.0f + 1e-6f) << n;
                }
            }
        }
    }
}

TEST(LookaheadLimiter, ALimitedSineKeepsItsShape) {
    // A steady sine 6 dB over the ceiling comes out as the same sine, scaled: no clipping.
    TestLimiter limiter;
    limiter.set(P::Ceiling, -6.0f);
    limiter.set(P::Release, 500.0f);
    limiter.prepare(k_sample_rate, k_block, 1);
    Buffer<float> signal = sine(1, 48000, 997.0, 1.0f);
    const Buffer<float> input = signal;
    run(limiter, signal);
    const size_t delay = limiter.latency_samples();
    float lowest = 10.0f;
    float highest = 0.0f;
    for (size_t n = 24000; n < 48000; ++n) {
        const float x = input.get_read_pointer(0)[n - delay];
        if (std::abs(x) < 0.1f) { continue; }
        const float ratio = signal.get_read_pointer(0)[n] / x;
        lowest = std::min(lowest, ratio);
        highest = std::max(highest, ratio);
    }
    EXPECT_NEAR(db(highest), -6.0f, 0.01f);
    EXPECT_LT(db(highest) - db(lowest), 0.05f);  // one gain, not a clipped waveform
}

TEST(LookaheadLimiter, TruePeakCatchesThePeaksBetweenSamples) {
    // fs / 4 at 45 degrees: the samples read 0.707 of the sine's peak of 1.
    const auto output_peak = [](bool true_peak) {
        TestLimiter limiter;
        limiter.set(P::Ceiling, -1.0f);
        limiter.set(P::TruePeak, true_peak ? 1.0f : 0.0f);
        limiter.set(P::Release, 500.0f);
        limiter.prepare(k_sample_rate, k_block, 1);
        Buffer<float> signal = sine(1, 48000, k_sample_rate / 4.0, 1.0f, std::numbers::pi / 4.0);
        run(limiter, signal);
        EXPECT_EQ(limiter.latency_samples(),
                  95u + (true_peak ? LookaheadLimiterImpl::k_true_peak_delay : 0u));
        float peak = 0.0f;
        for (size_t n = 24000; n < 48000; ++n) {
            peak = std::max(peak, std::abs(signal.get_read_pointer(0)[n]));
        }
        return peak * std::numbers::sqrt2_v<float>;  // the sine's peak, between the samples
    };
    EXPECT_NEAR(db(output_peak(false)), 0.0f, 0.01f);  // untouched: the samples are under -1 dB
    EXPECT_NEAR(db(output_peak(true)), -1.0f, 0.1f);
}

TEST(LookaheadLimiter, ReleaseRecoversInDecibels) {
    TestLimiter limiter;
    limiter.set(P::Ceiling, 0.0f);
    limiter.set(P::Lookahead, 1.0f);
    limiter.set(P::Release, 50.0f);
    limiter.prepare(k_sample_rate, k_block, 1);
    Buffer<float> signal(1, 9600);
    std::fill_n(signal.get_write_pointer(0), 4800, 4.0f);         // 12 dB over
    std::fill_n(signal.get_write_pointer(0) + 4800, 4800, 0.1f);  // far under
    run(limiter, signal);
    const size_t delay = limiter.latency_samples();
    // 50 ms after the loud part's minimum left the window (48 samples), 1/e of the 12 dB remains.
    const size_t at = 4800 + 48 + 2400 + delay - 1;
    EXPECT_NEAR(db(signal.get_read_pointer(0)[at] / 0.1f), -12.04f * std::exp(-1.0f), 0.15f);
}

TEST(LookaheadLimiter, LinkedSharesTheGainDualMonoDoesNot) {
    const auto quiet_channel_gain_db = [](StereoLink link) {
        TestLimiter limiter;
        limiter.set(P::Ceiling, 0.0f);
        limiter.set(P::Link, static_cast<float>(link));
        limiter.prepare(k_sample_rate, k_block, 2);
        Buffer<float> signal(2, 4800);
        std::fill_n(signal.get_write_pointer(0), 4800, 2.0f);
        std::fill_n(signal.get_write_pointer(1), 4800, 0.5f);
        run(limiter, signal);
        return db(signal.get_read_pointer(1)[4799] / 0.5f);
    };
    EXPECT_NEAR(quiet_channel_gain_db(StereoLink::Linked), -6.02f, 1e-3f);
    EXPECT_NEAR(quiet_channel_gain_db(StereoLink::DualMono), 0.0f, 1e-5f);
}

TEST(LookaheadLimiter, InputGainDrivesIntoTheCeiling) {
    TestLimiter limiter;
    limiter.set(P::Ceiling, -3.0f);
    limiter.set(P::InputGain, 12.0f);
    limiter.prepare(k_sample_rate, k_block, 1);
    Buffer<float> signal(1, 4800);
    std::fill_n(signal.get_write_pointer(0), 4800, amplitude(-20.0f));
    run(limiter, signal);
    EXPECT_NEAR(db(signal.get_read_pointer(0)[4799]), -8.0f, 1e-3f);  // -20 + 12, under -3
    limiter.set(P::InputGain, 24.0f);
    std::fill_n(signal.get_write_pointer(0), 4800, amplitude(-20.0f));
    run(limiter, signal);
    EXPECT_NEAR(db(signal.get_read_pointer(0)[4799]), -3.0f, 1e-3f);
    EXPECT_NEAR(limiter.gain_reduction_db(), -7.0f, 1e-3f);  // +4 dB to -3 dB
    EXPECT_NEAR(limiter.output_peak_db(), -3.0f, 1e-3f);
}

TEST(LookaheadLimiter, ALookaheadChangeMovesTheLatency) {
    TestLimiter limiter;
    limiter.set(P::Lookahead, 1.0f);
    limiter.prepare(k_sample_rate, k_block, 1);
    EXPECT_EQ(limiter.latency_samples(), 47u);
    Buffer<float> signal = bursts(1, 4800, 3);
    run(limiter, signal);
    limiter.set(P::Lookahead, 4.0f);
    signal = bursts(1, 4800, 4);
    run(limiter, signal);
    EXPECT_EQ(limiter.latency_samples(), 191u);
    for (size_t n = 0; n < 4800; ++n) {
        ASSERT_LE(std::abs(signal.get_read_pointer(0)[n]), amplitude(-1.0f));
    }
}

TEST(LookaheadLimiter, ResetClearsTheDelayAndTheGain) {
    TestLimiter limiter;
    limiter.set(P::Release, 2000.0f);
    limiter.prepare(k_sample_rate, k_block, 1);
    Buffer<float> loud = bursts(1, 4800, 5);
    run(limiter, loud);
    limiter.reset();
    Buffer<float> quiet(1, 960);
    std::fill_n(quiet.get_write_pointer(0), 960, 0.25f);
    run(limiter, quiet);
    for (size_t n = 0; n < 95; ++n) { ASSERT_EQ(quiet.get_read_pointer(0)[n], 0.0f) << n; }
    for (size_t n = 95; n < 960; ++n) { ASSERT_EQ(quiet.get_read_pointer(0)[n], 0.25f) << n; }
}

TEST(LookaheadLimiter, ParametersAreReadAtTheChangePoints) {
    TestLimiter limiter;
    limiter.prepare(k_sample_rate, k_block, 1);
    limiter.m_num_offsets = 0;
    Buffer<float> block(1, k_block);
    const std::array<uint32_t, 2> change_points{16, 40};
    limiter.process_modulated(BufferView(block), change_points);
    ASSERT_EQ(limiter.m_num_offsets, 3u);
    EXPECT_EQ(limiter.m_offsets[1], 16u);
    EXPECT_EQ(limiter.m_offsets[2], 40u);
}

/// The output's true peak in dB, measured 16x, for noise bursts low-passed at @p cutoff.
static float output_true_peak_db(double cutoff, bool true_peak, unsigned seed) {
    TestLimiter limiter;
    limiter.set(P::Ceiling, -1.0f);
    limiter.set(P::TruePeak, true_peak ? 1.0f : 0.0f);
    limiter.set(P::Release, 30.0f);
    limiter.prepare(k_sample_rate, k_block, 1);
    const Buffer<float> white = bursts(1, 24000, seed);
    Buffer<float> signal(1, 24000);
    constexpr int k_taps_half = 127;
    const double fc = cutoff / k_sample_rate;
    for (int n = 0; n < 24000; ++n) {
        double value = 0.0;
        for (int j = -k_taps_half; j <= k_taps_half; ++j) {
            if (n - j < 0 || n - j >= 24000) { continue; }
            const double sinc =
                j == 0 ? 2.0 * fc
                       : std::sin(2.0 * std::numbers::pi * fc * j) / (std::numbers::pi * j);
            const double window = 0.5 + 0.5 * std::cos(std::numbers::pi * j / (k_taps_half + 1));
            value += white.get_read_pointer(0)[n - j] * sinc * window;
        }
        signal.get_write_pointer(0)[n] = static_cast<float>(value);
    }
    run(limiter, signal);
    const float* y = signal.get_read_pointer(0);
    constexpr int k_factor = 16;
    constexpr int k_half = 64;
    float peak = 0.0f;
    for (int n = 2000; n < 23000; ++n) {
        for (int p = 0; p < k_factor; ++p) {
            const double t = static_cast<double>(p) / k_factor;
            double value = 0.0;
            for (int j = -k_half + 1; j <= k_half; ++j) {
                const double d = t - j;
                if (d == 0.0) {
                    value += y[n + j];
                    continue;
                }
                const double window = 0.5 + 0.5 * std::cos(std::numbers::pi * d / (k_half + 1));
                value +=
                    y[n + j] * window * std::sin(std::numbers::pi * d) / (std::numbers::pi * d);
            }
            peak = std::max(peak, static_cast<float>(std::abs(value)));
        }
    }
    return db(peak);
}

TEST(LookaheadLimiter, TruePeakHoldsTheCeilingOnBandLimitedNoise) {
    // Noise bursts with content up to 20 kHz; the output's true peak measured 16x.
    for (const unsigned seed : {1u, 2u}) {
        EXPECT_LT(output_true_peak_db(20000.0, true, seed), -0.7f) << seed;
        EXPECT_GT(output_true_peak_db(20000.0, false, seed), 0.0f) << seed;  // sample peaks miss it
    }
}
