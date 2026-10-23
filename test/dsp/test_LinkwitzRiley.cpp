#include <gtest/gtest.h>
#include <tanh/dsp/utils/Butterworth.h>
#include <tanh/dsp/utils/LinkwitzRiley.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <utility>
#include <vector>

using thl::dsp::utils::Butterworth;
using thl::dsp::utils::FilterType;
using thl::dsp::utils::LinkwitzRiley;

namespace {

constexpr float k_sample_rate = 48000.0f;
constexpr float k_cutoff = 1000.0f;
constexpr size_t k_impulse_length = 1 << 14;

enum class Output : uint8_t { Low, High, Sum };

// |H(f)| of a band or of their sum, from the impulse response.
double output_gain(size_t order, Output output, double frequency) {
    LinkwitzRiley crossover;
    crossover.prepare(k_cutoff, k_sample_rate, order);
    const double w = 2.0 * std::numbers::pi * frequency / k_sample_rate;
    std::complex<double> sum = 0.0;
    for (size_t n = 0; n < k_impulse_length; ++n) {
        float low = 0.0f;
        float high = 0.0f;
        crossover.process(n == 0 ? 1.0f : 0.0f, low, high);
        float value = low + high;
        if (output == Output::Low) { value = low; }
        if (output == Output::High) { value = high; }
        sum += static_cast<double>(value) * std::polar(1.0, -w * static_cast<double>(n));
    }
    return std::abs(sum);
}

double output_gain_db(size_t order, Output output, double frequency) {
    return 20.0 * std::log10(output_gain(order, output, frequency));
}

// The squared Butterworth of half the order, prewarped at the cutoff.
double expected_band_gain(size_t order, Output output, double frequency) {
    double ratio = std::tan(std::numbers::pi * frequency / k_sample_rate) /
                   std::tan(std::numbers::pi * k_cutoff / k_sample_rate);
    if (output == Output::High) { ratio = 1.0 / ratio; }
    return 1.0 / (1.0 + std::pow(ratio, static_cast<double>(order)));
}

std::pair<std::vector<float>, std::vector<float>> impulse_responses(LinkwitzRiley& crossover,
                                                                    size_t length) {
    std::vector<float> low(length);
    std::vector<float> high(length);
    for (size_t n = 0; n < length; ++n) {
        crossover.process(n == 0 ? 1.0f : 0.0f, low[n], high[n]);
    }
    return {low, high};
}

}  // namespace

TEST(LinkwitzRiley, SetFrequencyMatchesPrepare) {
    for (size_t order = 2; order <= LinkwitzRiley::k_max_order; order += 2) {
        SCOPED_TRACE(order);
        LinkwitzRiley changed;
        LinkwitzRiley prepared;
        changed.prepare(500.0f, k_sample_rate, order);
        changed.set_frequency(2000.0f);
        prepared.prepare(2000.0f, k_sample_rate, order);
        EXPECT_EQ(impulse_responses(changed, 4096), impulse_responses(prepared, 4096));
    }
}

TEST(LinkwitzRiley, ProcessWithPrewarpedGainMatchesSetFrequency) {
    for (size_t order = 2; order <= LinkwitzRiley::k_max_order; order += 2) {
        SCOPED_TRACE(order);
        // prepared at another frequency, to show that process_low/high(x, g) ignore it
        LinkwitzRiley given;
        LinkwitzRiley prepared;
        given.prepare(500.0f, k_sample_rate, order);
        prepared.prepare(2000.0f, k_sample_rate, order);
        const float g = Butterworth::prewarp(2000.0f, k_sample_rate);
        std::vector<float> low(4096);
        std::vector<float> high(4096);
        for (size_t n = 0; n < low.size(); ++n) {
            const float x = n == 0 ? 1.0f : 0.0f;
            low[n] = given.process_low(x, g);
            high[n] = given.process_high(x, g);
        }
        EXPECT_EQ(std::make_pair(low, high), impulse_responses(prepared, 4096));
    }
}

TEST(LinkwitzRiley, ResetClearsTheState) {
    for (size_t order = 2; order <= LinkwitzRiley::k_max_order; order += 2) {
        SCOPED_TRACE(order);
        LinkwitzRiley used;
        LinkwitzRiley fresh;
        used.prepare(k_cutoff, k_sample_rate, order);
        fresh.prepare(k_cutoff, k_sample_rate, order);
        impulse_responses(used, 4096);
        used.reset();
        EXPECT_EQ(impulse_responses(used, 4096), impulse_responses(fresh, 4096));
    }
}

TEST(LinkwitzRiley, BandsAreSixDecibelsDownAtTheCutoff) {
    for (size_t order = 2; order <= LinkwitzRiley::k_max_order; order += 2) {
        SCOPED_TRACE(order);
        EXPECT_NEAR(output_gain_db(order, Output::Low, k_cutoff), -6.0206, 1e-3);
        EXPECT_NEAR(output_gain_db(order, Output::High, k_cutoff), -6.0206, 1e-3);
    }
}

TEST(LinkwitzRiley, BandsMatchTheSquaredButterworthMagnitude) {
    for (size_t order = 2; order <= LinkwitzRiley::k_max_order; order += 2) {
        for (const Output output : {Output::Low, Output::High}) {
            for (const double frequency : {350.0, 500.0, 700.0, 1000.0, 1400.0, 2000.0, 2800.0}) {
                SCOPED_TRACE(testing::Message() << "order " << order << ", " << frequency << " Hz");
                EXPECT_NEAR(output_gain_db(order, output, frequency),
                            20.0 * std::log10(expected_band_gain(order, output, frequency)),
                            0.01);
            }
        }
    }
}

TEST(LinkwitzRiley, BandsSumToAnAllPass) {
    for (size_t order = 2; order <= LinkwitzRiley::k_max_order; order += 2) {
        SCOPED_TRACE(order);
        LinkwitzRiley crossover;
        Butterworth all_pass;
        crossover.prepare(k_cutoff, k_sample_rate, order);
        all_pass.prepare(k_cutoff, k_sample_rate, order / 2, FilterType::AllPass);

        float max_difference = 0.0f;
        for (size_t n = 0; n < 4096; ++n) {
            const float x = n == 0 ? 1.0f : 0.0f;
            float low = 0.0f;
            float high = 0.0f;
            crossover.process(x, low, high);
            max_difference = std::max(max_difference, std::abs(all_pass.process(x) - (low + high)));
        }
        EXPECT_LT(max_difference, 1e-5f);
    }
}

TEST(LinkwitzRiley, BandsSumToUnitGainEverywhere) {
    for (size_t order = 2; order <= LinkwitzRiley::k_max_order; order += 2) {
        for (const double frequency :
             {0.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, k_sample_rate / 2.0}) {
            SCOPED_TRACE(testing::Message() << "order " << order << ", " << frequency << " Hz");
            EXPECT_NEAR(output_gain(order, Output::Sum, frequency), 1.0, 1e-4);
        }
    }
}
