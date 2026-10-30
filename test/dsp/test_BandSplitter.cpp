#include <gtest/gtest.h>
#include <tanh/core/Buffer.h>
#include <tanh/core/BufferView.h>
#include <tanh/dsp/filter/Butterworth.h>
#include <tanh/dsp/filter/LinkwitzRiley.h>
#include <tanh/dsp/utils/BandSplitter.h>
#include <tanh/dsp/utils/SmoothedValue.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <random>
#include <vector>

using thl::core::Buffer;
using thl::core::BufferView;
using thl::dsp::filter::Butterworth;
using thl::dsp::filter::LinkwitzRiley;
using thl::dsp::utils::BandSplitter;
using thl::dsp::utils::SmoothedValue;

namespace {

constexpr float k_sample_rate = 48000.0f;
constexpr size_t k_block_size = 64;

// Makes process_band() reachable; the tests call it directly instead of process().
class TestBandSplitter : public BandSplitter {
public:
    using BandSplitter::BandSplitter;
    using BandSplitter::process_band;

    void process(BufferView /*buffer*/,
                 uint32_t /*modulation_offset*/) TANH_NONBLOCKING_FUNCTION override {}
};

using Bands = std::vector<Buffer<float>>;

Buffer<float> noise(size_t num_channels, size_t num_samples) {
    std::mt19937 generator(1);
    std::uniform_real_distribution<float> distribution(-1.0f, 1.0f);
    Buffer<float> signal(num_channels, num_samples);
    for (size_t channel = 0; channel < num_channels; ++channel) {
        float* samples = signal.get_write_pointer(channel);
        for (size_t n = 0; n < num_samples; ++n) { samples[n] = distribution(generator); }
    }
    return signal;
}

Buffer<float> sine(float frequency, size_t num_samples) {
    Buffer<float> signal(1, num_samples);
    float* samples = signal.get_write_pointer(0);
    for (size_t n = 0; n < num_samples; ++n) {
        samples[n] = std::sin(2.0f * std::numbers::pi_v<float> * frequency * static_cast<float>(n) /
                              k_sample_rate);
    }
    return signal;
}

// Processes samples start to start + length - 1 of every band; band 0 holds the input there.
void process_block(TestBandSplitter& splitter, Bands& bands, size_t start, size_t length) {
    for (size_t band = 0; band < bands.size(); ++band) {
        splitter.process_band(BufferView(bands[band]).sub_block(start, length), band);
    }
}

// Splits the signal in blocks of k_block_size.
Bands split(TestBandSplitter& splitter, const Buffer<float>& signal) {
    Bands bands(splitter.num_bands(), signal);
    const size_t num_samples = signal.get_num_samples();
    for (size_t start = 0; start < num_samples; start += k_block_size) {
        process_block(splitter, bands, start, std::min(k_block_size, num_samples - start));
    }
    return bands;
}

// The largest difference between the sum of the bands and the reference.
float max_sum_error(const Bands& bands, const Buffer<float>& reference) {
    float max_error = 0.0f;
    for (size_t channel = 0; channel < reference.get_num_channels(); ++channel) {
        for (size_t n = 0; n < reference.get_num_samples(); ++n) {
            float sum = 0.0f;
            for (const Buffer<float>& band : bands) { sum += band.get_read_pointer(channel)[n]; }
            max_error = std::max(max_error, std::abs(sum - reference.get_read_pointer(channel)[n]));
        }
    }
    return max_error;
}

// RMS of the second half, after the filters have settled, relative to a full-scale sine.
float settled_gain_db(const Buffer<float>& band) {
    const size_t num_samples = band.get_num_samples();
    const float* samples = band.get_read_pointer(0);
    double sum_of_squares = 0.0;
    for (size_t n = num_samples / 2; n < num_samples; ++n) {
        sum_of_squares += static_cast<double>(samples[n]) * static_cast<double>(samples[n]);
    }
    const double rms =
        std::sqrt(sum_of_squares / static_cast<double>(num_samples - num_samples / 2));
    return static_cast<float>(20.0 * std::log10(rms * std::numbers::sqrt2));
}

}  // namespace

TEST(BandSplitter, BandsSumToTheAllPassesOfAllCrossovers) {
    const Buffer<float> signal = noise(2, 8192);
    for (size_t num_bands = 2; num_bands <= 4; ++num_bands) {
        for (size_t order = 2; order <= LinkwitzRiley::k_max_order; order += 2) {
            SCOPED_TRACE(testing::Message() << num_bands << " bands, order " << order);
            TestBandSplitter splitter(num_bands, order);
            splitter.prepare(k_sample_rate, k_block_size, signal.get_num_channels());
            const Bands bands = split(splitter, signal);

            Buffer<float> reference = signal;
            for (size_t channel = 0; channel < reference.get_num_channels(); ++channel) {
                float* samples = reference.get_write_pointer(channel);
                for (size_t crossover = 0; crossover + 1 < num_bands; ++crossover) {
                    Butterworth all_pass;
                    all_pass.prepare(splitter.get_frequency(crossover),
                                     k_sample_rate,
                                     order / 2,
                                     Butterworth::Response::AllPass);
                    for (size_t n = 0; n < reference.get_num_samples(); ++n) {
                        samples[n] = all_pass.process(samples[n]);
                    }
                }
            }
            EXPECT_LT(max_sum_error(bands, reference), 1e-5f);
        }
    }
}

TEST(BandSplitter, EachBandHoldsItsRange) {
    // the default crossovers of 4 bands: 100 Hz, 1 kHz and 10 kHz
    const std::array<float, 4> frequencies = {30.0f, 316.0f, 3162.0f, 18000.0f};
    for (size_t sine_band = 0; sine_band < frequencies.size(); ++sine_band) {
        SCOPED_TRACE(testing::Message() << frequencies[sine_band] << " Hz");
        TestBandSplitter splitter(4, 4);
        splitter.prepare(k_sample_rate, k_block_size, 1);
        const Bands bands = split(splitter, sine(frequencies[sine_band], 48000));
        for (size_t band = 0; band < bands.size(); ++band) {
            SCOPED_TRACE(testing::Message() << "band " << band);
            if (band == sine_band) {
                EXPECT_GT(settled_gain_db(bands[band]), -1.0f);
            } else {
                EXPECT_LT(settled_gain_db(bands[band]), -30.0f);
            }
        }
    }
}

TEST(BandSplitter, SetFrequencyClampsBetweenTheNeighbours) {
    // crossovers at 100 Hz, 1 kHz and 10 kHz
    TestBandSplitter splitter(4, 4);
    splitter.prepare(k_sample_rate, k_block_size, 1);

    splitter.set_frequency(1, 20000.0f);
    EXPECT_FLOAT_EQ(splitter.get_frequency(1), 10000.0f);
    splitter.set_frequency(1, 50.0f);
    EXPECT_FLOAT_EQ(splitter.get_frequency(1), 100.0f);
    splitter.set_frequency(0, -10.0f);
    EXPECT_FLOAT_EQ(splitter.get_frequency(0), 0.0f);
    splitter.set_frequency(2, 30000.0f);
    EXPECT_FLOAT_EQ(splitter.get_frequency(2), Butterworth::k_max_frequency_ratio * k_sample_rate);
}

TEST(BandSplitter, PrepareClampsBelowNyquist) {
    // crossovers at 100 Hz, 1 kHz and 10 kHz, the last above Nyquist at 16 kHz
    TestBandSplitter splitter(4, 4);
    splitter.prepare(16000.0, k_block_size, 1);
    EXPECT_FLOAT_EQ(splitter.get_frequency(0), 100.0f);
    EXPECT_FLOAT_EQ(splitter.get_frequency(1), 1000.0f);
    EXPECT_FLOAT_EQ(splitter.get_frequency(2), Butterworth::k_max_frequency_ratio * 16000.0f);
}

TEST(BandSplitter, GlidingCrossoversStillSumToTheAllPasses) {
    constexpr size_t k_order = 4;
    constexpr size_t k_glide_start = 4 * k_block_size;
    const std::array<float, 2> targets = {2000.0f, 5000.0f};
    const Buffer<float> signal = noise(2, 8192);

    TestBandSplitter splitter(3, k_order);
    splitter.prepare(k_sample_rate, k_block_size, signal.get_num_channels());
    const std::array<float, 2> starts = {splitter.get_frequency(0), splitter.get_frequency(1)};
    Bands bands(splitter.num_bands(), signal);
    for (size_t start = 0; start < signal.get_num_samples(); start += k_block_size) {
        if (start == k_glide_start) {
            splitter.set_frequency(0, targets[0]);
            splitter.set_frequency(1, targets[1]);
        }
        process_block(splitter, bands, start, k_block_size);
    }

    // the reference all-passes follow the same glides
    Buffer<float> reference = signal;
    for (size_t channel = 0; channel < reference.get_num_channels(); ++channel) {
        float* samples = reference.get_write_pointer(channel);
        for (size_t crossover = 0; crossover < targets.size(); ++crossover) {
            SmoothedValue frequency;
            frequency.reset(k_sample_rate, BandSplitter::k_smoothing_seconds);
            frequency.set_current_and_target_value(starts[crossover]);
            Butterworth all_pass;
            all_pass.prepare(starts[crossover],
                             k_sample_rate,
                             k_order / 2,
                             Butterworth::Response::AllPass);
            for (size_t n = 0; n < reference.get_num_samples(); ++n) {
                if (n == k_glide_start) { frequency.set_target_value(targets[crossover]); }
                const float g = Butterworth::prewarp(frequency.get_smoothed_value(), k_sample_rate);
                samples[n] = all_pass.process(samples[n], g);
            }
        }
    }
    EXPECT_LT(max_sum_error(bands, reference), 1e-5f);
}

#if !defined(NDEBUG) && GTEST_HAS_DEATH_TEST
TEST(BandSplitterDeathTest, AssertsTheBandOrder) {
    TestBandSplitter splitter(3, 4);
    splitter.prepare(k_sample_rate, k_block_size, 1);
    Buffer<float> buffer(1, k_block_size);
    // RTSan off for the call that asserts: its abort would be reported before the message.
    EXPECT_DEATH(
        {
            TANH_NONBLOCKING_SCOPED_DISABLER
            splitter.process_band(BufferView(buffer), 1);
        },
        "m_next_band");
}
#endif
