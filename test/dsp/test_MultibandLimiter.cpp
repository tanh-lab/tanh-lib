#include <gtest/gtest.h>
#include <tanh/core/Buffer.h>
#include <tanh/core/BufferView.h>
#include <tanh/dsp/filter/LinkwitzRiley.h>
#include <tanh/dsp/utils/MultibandLimiter.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <random>
#include <vector>

using thl::core::Buffer;
using thl::core::BufferView;
using thl::dsp::filter::LinkwitzRiley;
using thl::dsp::utils::MultibandLimiterImpl;

namespace {

constexpr float k_sample_rate = 48000.0f;
constexpr size_t k_block_size = 64;

class TestMultibandLimiter : public MultibandLimiterImpl {
public:
    TestMultibandLimiter(size_t num_bands, size_t order)
        : MultibandLimiterImpl(num_bands, order), m_thresholds_db(num_bands, 0.0f) {}

    std::vector<float> m_thresholds_db;
    float m_attack_ms = 1.0f;
    float m_release_ms = 50.0f;

protected:
    float get_band_parameter_float(BandParameter parameter,
                                   size_t band,
                                   uint32_t /*modulation_offset*/) override {
        switch (parameter) {
            case Threshold: return m_thresholds_db[band];
            case Attack: return m_attack_ms;
            case Release: return m_release_ms;
            case NumBandParameters: break;
        }
        return 0.0f;
    }
};

Buffer<float> noise(size_t num_channels, size_t num_samples, float amplitude) {
    std::mt19937 generator(1);
    std::uniform_real_distribution<float> distribution(-amplitude, amplitude);
    Buffer<float> signal(num_channels, num_samples);
    for (size_t channel = 0; channel < num_channels; ++channel) {
        float* samples = signal.get_write_pointer(channel);
        for (size_t n = 0; n < num_samples; ++n) { samples[n] = distribution(generator); }
    }
    return signal;
}

Buffer<float> sine(float frequency, float amplitude, size_t num_samples) {
    Buffer<float> signal(1, num_samples);
    float* samples = signal.get_write_pointer(0);
    for (size_t n = 0; n < num_samples; ++n) {
        samples[n] = amplitude * std::sin(2.0f * std::numbers::pi_v<float> * frequency *
                                          static_cast<float>(n) / k_sample_rate);
    }
    return signal;
}

void process(TestMultibandLimiter& limiter, Buffer<float>& signal) {
    const size_t num_samples = signal.get_num_samples();
    for (size_t start = 0; start < num_samples; start += k_block_size) {
        limiter.process(
            BufferView(signal).sub_block(start, std::min(k_block_size, num_samples - start)));
    }
}

void process(TestMultibandLimiter& limiter, Buffer<float>& signal, Buffer<float>& dry) {
    const size_t num_samples = signal.get_num_samples();
    for (size_t start = 0; start < num_samples; start += k_block_size) {
        const size_t length = std::min(k_block_size, num_samples - start);
        limiter.process_with_dry(BufferView(signal).sub_block(start, length),
                                 0,
                                 BufferView(dry).sub_block(start, length));
    }
}

// The peak of the second half, after the limiters have settled.
float settled_peak(const Buffer<float>& signal) {
    const size_t num_samples = signal.get_num_samples();
    const float* samples = signal.get_read_pointer(0);
    float peak = 0.0f;
    for (size_t n = num_samples / 2; n < num_samples; ++n) {
        peak = std::max(peak, std::abs(samples[n]));
    }
    return peak;
}

}  // namespace

TEST(MultibandLimiter, BelowTheThresholdsTheOutputIsTheDrySignal) {
    const Buffer<float> signal = noise(2, 8192, 0.1f);
    for (size_t num_bands = 2; num_bands <= 4; ++num_bands) {
        for (size_t order = 2; order <= LinkwitzRiley::k_max_order; order += 2) {
            SCOPED_TRACE(testing::Message() << num_bands << " bands, order " << order);
            TestMultibandLimiter limiter(num_bands, order);
            limiter.prepare(k_sample_rate, k_block_size, signal.get_num_channels());

            Buffer<float> output = signal;
            Buffer<float> dry(signal.get_num_channels(), signal.get_num_samples());
            process(limiter, output, dry);

            float max_error = 0.0f;
            for (size_t channel = 0; channel < signal.get_num_channels(); ++channel) {
                for (size_t n = 0; n < signal.get_num_samples(); ++n) {
                    max_error = std::max(max_error,
                                         std::abs(output.get_read_pointer(channel)[n] -
                                                  dry.get_read_pointer(channel)[n]));
                }
            }
            EXPECT_LT(max_error, 1e-5f);
        }
    }
}

TEST(MultibandLimiter, LimitsTheBandHoldingTheSignal) {
    // crossovers at 100 Hz and 10 kHz: 1 kHz is in band 1
    TestMultibandLimiter limiter(3, 4);
    limiter.m_thresholds_db[1] = -20.0f;
    limiter.prepare(k_sample_rate, k_block_size, 1);

    Buffer<float> signal = sine(1000.0f, 0.5f, 48000);
    process(limiter, signal);
    EXPECT_NEAR(settled_peak(signal), 0.1f, 0.02f);
}

TEST(MultibandLimiter, LeavesTheOtherBandsAlone) {
    // crossovers at 100 Hz and 10 kHz: 1 kHz is in band 1, limited only in band 0
    TestMultibandLimiter limiter(3, 4);
    limiter.m_thresholds_db[0] = -20.0f;
    limiter.prepare(k_sample_rate, k_block_size, 1);

    Buffer<float> signal = sine(1000.0f, 0.5f, 48000);
    process(limiter, signal);
    EXPECT_NEAR(settled_peak(signal), 0.5f, 0.01f);
}

TEST(MultibandLimiter, ResetReleasesTheLimiters) {
    TestMultibandLimiter limiter(2, 4);
    std::fill(limiter.m_thresholds_db.begin(), limiter.m_thresholds_db.end(), -20.0f);
    limiter.m_release_ms = 1000.0f;  // would take long to recover on its own
    limiter.prepare(k_sample_rate, k_block_size, 1);
    Buffer<float> loud = sine(1000.0f, 1.0f, 9600);
    process(limiter, loud);
    limiter.reset();

    // Right after the reset a quiet signal passes as the dry signal (the bands' sum).
    Buffer<float> quiet = sine(1000.0f, 0.01f, 4096);
    Buffer<float> dry(1, quiet.get_num_samples());
    process(limiter, quiet, dry);
    for (size_t n = 0; n < quiet.get_num_samples(); ++n) {
        ASSERT_NEAR(quiet.get_read_pointer(0)[n], dry.get_read_pointer(0)[n], 1e-6f) << n;
    }
}

namespace {

/// Its crossovers come from parameters, read with the block position.
class ParameterisedMultibandLimiter : public TestMultibandLimiter {
public:
    ParameterisedMultibandLimiter() : TestMultibandLimiter(4, 4) { m_offsets.reserve(64); }

    std::array<float, 3> m_crossovers{200.0f, 2000.0f, 9000.0f};
    std::vector<uint32_t> m_offsets;  // reserved: process() runs under RTSan

protected:
    float get_crossover_parameter_float(size_t crossover, uint32_t modulation_offset) override {
        if (crossover == 0) { m_offsets.push_back(modulation_offset); }
        return m_crossovers[crossover];
    }
};

}  // namespace

TEST(MultibandLimiter, CrossoversComeFromTheirParameters) {
    ParameterisedMultibandLimiter limiter;
    limiter.prepare(k_sample_rate, k_block_size, 1);
    Buffer<float> block(1, k_block_size);
    limiter.process(BufferView(block));
    EXPECT_FLOAT_EQ(limiter.get_frequency(0), 200.0f);
    EXPECT_FLOAT_EQ(limiter.get_frequency(1), 2000.0f);
    EXPECT_FLOAT_EQ(limiter.get_frequency(2), 9000.0f);

    limiter.m_crossovers = {5000.0f, 300.0f, 9000.0f};  // crossing: sorted, whatever the order
    limiter.process(BufferView(block));
    EXPECT_FLOAT_EQ(limiter.get_frequency(0), 300.0f);
    EXPECT_FLOAT_EQ(limiter.get_frequency(1), 5000.0f);
    EXPECT_FLOAT_EQ(limiter.get_frequency(2), 9000.0f);
}

TEST(MultibandLimiter, CrossoverParametersAreReadAtTheBlockPosition) {
    ParameterisedMultibandLimiter limiter;
    limiter.prepare(k_sample_rate, k_block_size, 1);
    Buffer<float> block(1, k_block_size);
    const std::array<uint32_t, 2> change_points{16, 40};
    limiter.process_modulated(BufferView(block), change_points);
    EXPECT_EQ(limiter.m_offsets, (std::vector<uint32_t>{0, 16, 40}));
}

TEST(MultibandLimiter, WithoutCrossoverParametersSetFrequencyDecides) {
    TestMultibandLimiter limiter(2, 4);
    limiter.prepare(k_sample_rate, k_block_size, 1);
    limiter.set_frequency(0, 700.0f);
    Buffer<float> block(1, k_block_size);
    limiter.process(BufferView(block));
    EXPECT_FLOAT_EQ(limiter.get_frequency(0), 700.0f);
}
