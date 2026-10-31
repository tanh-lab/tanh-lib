#include <gtest/gtest.h>
#include <tanh/dsp/filter/Butterworth.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>
#include <vector>

using thl::dsp::filter::Butterworth;

namespace {

constexpr float k_sample_rate = 48000.0f;
constexpr float k_cutoff = 1000.0f;
constexpr size_t k_impulse_length = 1 << 14;

// |H(f)| from the impulse response, so no settling time or sine window is involved.
double gain(size_t order,
            Butterworth::Response filter_type,
            double frequency,
            float cutoff = k_cutoff) {
    Butterworth filter;
    filter.prepare(cutoff, k_sample_rate, order, filter_type);
    const double w = 2.0 * std::numbers::pi * frequency / k_sample_rate;
    std::complex<double> sum = 0.0;
    for (size_t n = 0; n < k_impulse_length; ++n) {
        const float h = filter.process(n == 0 ? 1.0f : 0.0f);
        sum += static_cast<double>(h) * std::polar(1.0, -w * static_cast<double>(n));
    }
    return std::abs(sum);
}

double gain_db(size_t order,
               Butterworth::Response filter_type,
               double frequency,
               float cutoff = k_cutoff) {
    return 20.0 * std::log10(gain(order, filter_type, frequency, cutoff));
}

// The bilinear transform of the analog Butterworth, prewarped at the cutoff.
double expected_gain(size_t order, Butterworth::Response filter_type, double frequency) {
    double ratio = std::tan(std::numbers::pi * frequency / k_sample_rate) /
                   std::tan(std::numbers::pi * k_cutoff / k_sample_rate);
    if (filter_type == Butterworth::Response::HighPass) { ratio = 1.0 / ratio; }
    return 1.0 / std::sqrt(1.0 + std::pow(ratio, 2.0 * static_cast<double>(order)));
}

std::vector<float> impulse_response(Butterworth& filter, size_t length) {
    std::vector<float> response(length);
    for (size_t n = 0; n < length; ++n) { response[n] = filter.process(n == 0 ? 1.0f : 0.0f); }
    return response;
}

}  // namespace

TEST(Butterworth, SetFrequencyMatchesPrepare) {
    for (size_t order = 1; order <= Butterworth::k_max_order; ++order) {
        for (const Butterworth::Response filter_type : {Butterworth::Response::LowPass,
                                                        Butterworth::Response::HighPass,
                                                        Butterworth::Response::AllPass}) {
            SCOPED_TRACE(order);
            Butterworth changed;
            Butterworth prepared;
            changed.prepare(500.0f, k_sample_rate, order, filter_type);
            changed.set_frequency(2000.0f);
            prepared.prepare(2000.0f, k_sample_rate, order, filter_type);
            EXPECT_EQ(impulse_response(changed, 4096), impulse_response(prepared, 4096));
        }
    }
}

TEST(Butterworth, ProcessWithPrewarpedGainMatchesSetFrequency) {
    for (size_t order = 1; order <= Butterworth::k_max_order; ++order) {
        for (const Butterworth::Response filter_type : {Butterworth::Response::LowPass,
                                                        Butterworth::Response::HighPass,
                                                        Butterworth::Response::AllPass}) {
            SCOPED_TRACE(order);
            // prepared at another frequency, to show that process(x, g) ignores it
            Butterworth given;
            Butterworth prepared;
            given.prepare(500.0f, k_sample_rate, order, filter_type);
            prepared.prepare(2000.0f, k_sample_rate, order, filter_type);
            const float g = Butterworth::prewarp(2000.0f, k_sample_rate);
            std::vector<float> response(4096);
            for (size_t n = 0; n < response.size(); ++n) {
                response[n] = given.process(n == 0 ? 1.0f : 0.0f, g);
            }
            EXPECT_EQ(response, impulse_response(prepared, 4096));
        }
    }
}

TEST(Butterworth, ResetClearsTheState) {
    for (size_t order = 1; order <= Butterworth::k_max_order; ++order) {
        SCOPED_TRACE(order);
        Butterworth used;
        Butterworth fresh;
        used.prepare(k_cutoff, k_sample_rate, order, Butterworth::Response::LowPass);
        fresh.prepare(k_cutoff, k_sample_rate, order, Butterworth::Response::LowPass);
        impulse_response(used, 4096);
        used.reset();
        EXPECT_EQ(impulse_response(used, 4096), impulse_response(fresh, 4096));
    }
}

TEST(Butterworth, ClampsTheCutoffBelowNyquist) {
    const float limit = Butterworth::k_max_frequency_ratio * k_sample_rate;
    for (size_t order = 1; order <= Butterworth::k_max_order; ++order) {
        for (const Butterworth::Response filter_type : {Butterworth::Response::LowPass,
                                                        Butterworth::Response::HighPass,
                                                        Butterworth::Response::AllPass}) {
            SCOPED_TRACE(order);
            Butterworth at_nyquist;
            Butterworth above_nyquist;
            Butterworth at_limit;
            at_nyquist.prepare(k_sample_rate / 2.0f, k_sample_rate, order, filter_type);
            above_nyquist.prepare(0.6f * k_sample_rate, k_sample_rate, order, filter_type);
            at_limit.prepare(limit, k_sample_rate, order, filter_type);
            const std::vector<float> expected = impulse_response(at_limit, 4096);
            EXPECT_EQ(impulse_response(at_nyquist, 4096), expected);
            EXPECT_EQ(impulse_response(above_nyquist, 4096), expected);
        }
    }
}

TEST(Butterworth, PassesTheBandAndStopsTheOtherEnd) {
    const double nyquist = k_sample_rate / 2.0;
    for (size_t order = 1; order <= Butterworth::k_max_order; ++order) {
        SCOPED_TRACE(order);
        EXPECT_NEAR(gain(order, Butterworth::Response::LowPass, 0.0), 1.0, 1e-4);
        EXPECT_NEAR(gain(order, Butterworth::Response::LowPass, nyquist), 0.0, 1e-4);
        EXPECT_NEAR(gain(order, Butterworth::Response::HighPass, 0.0), 0.0, 1e-4);
        EXPECT_NEAR(gain(order, Butterworth::Response::HighPass, nyquist), 1.0, 1e-4);
    }
}

TEST(Butterworth, IsThreeDecibelsDownAtTheCutoff) {
    for (size_t order = 1; order <= Butterworth::k_max_order; ++order) {
        SCOPED_TRACE(order);
        EXPECT_NEAR(gain_db(order, Butterworth::Response::LowPass, k_cutoff), -3.0103, 1e-3);
        EXPECT_NEAR(gain_db(order, Butterworth::Response::HighPass, k_cutoff), -3.0103, 1e-3);
    }
}

TEST(Butterworth, FallsBySixDecibelsPerOctavePerOrder) {
    // Far from the cutoff and well below Nyquist, where the slope is close to its asymptote.
    for (size_t order = 1; order <= Butterworth::k_max_order; ++order) {
        SCOPED_TRACE(order);
        const double expected = -6.0206 * static_cast<double>(order);
        const double low = gain_db(order, Butterworth::Response::LowPass, 1600.0, 100.0f) -
                           gain_db(order, Butterworth::Response::LowPass, 800.0, 100.0f);
        const double high = gain_db(order, Butterworth::Response::HighPass, 62.5) -
                            gain_db(order, Butterworth::Response::HighPass, 125.0);
        EXPECT_NEAR(low, expected, 0.2);
        EXPECT_NEAR(high, expected, 0.2);
    }
}

TEST(Butterworth, AllPassHasUnitGainEverywhere) {
    for (size_t order = 1; order <= Butterworth::k_max_order; ++order) {
        for (const double frequency :
             {0.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, k_sample_rate / 2.0}) {
            SCOPED_TRACE(testing::Message() << "order " << order << ", " << frequency << " Hz");
            EXPECT_NEAR(gain(order, Butterworth::Response::AllPass, frequency), 1.0, 1e-4);
        }
    }
}

TEST(Butterworth, AllPassMatchesTheLinkwitzRileySum) {
    for (size_t order = 1; order <= Butterworth::k_max_order; ++order) {
        SCOPED_TRACE(order);
        Butterworth low_1;
        Butterworth low_2;
        Butterworth high_1;
        Butterworth high_2;
        Butterworth all_pass;
        low_1.prepare(k_cutoff, k_sample_rate, order, Butterworth::Response::LowPass);
        low_2.prepare(k_cutoff, k_sample_rate, order, Butterworth::Response::LowPass);
        high_1.prepare(k_cutoff, k_sample_rate, order, Butterworth::Response::HighPass);
        high_2.prepare(k_cutoff, k_sample_rate, order, Butterworth::Response::HighPass);
        all_pass.prepare(k_cutoff, k_sample_rate, order, Butterworth::Response::AllPass);

        const float high_sign = order % 2 == 1 ? -1.0f : 1.0f;
        float max_difference = 0.0f;
        for (size_t n = 0; n < 4096; ++n) {
            const float x = n == 0 ? 1.0f : 0.0f;
            const float sum =
                low_2.process(low_1.process(x)) + high_sign * high_2.process(high_1.process(x));
            max_difference = std::max(max_difference, std::abs(all_pass.process(x) - sum));
        }
        EXPECT_LT(max_difference, 1e-5f);
    }
}

TEST(Butterworth, MatchesTheWarpedButterworthMagnitude) {
    for (size_t order = 1; order <= Butterworth::k_max_order; ++order) {
        for (const Butterworth::Response filter_type :
             {Butterworth::Response::LowPass, Butterworth::Response::HighPass}) {
            for (const double frequency : {125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0}) {
                SCOPED_TRACE(testing::Message() << "order " << order << ", " << frequency << " Hz");
                EXPECT_NEAR(gain_db(order, filter_type, frequency),
                            20.0 * std::log10(expected_gain(order, filter_type, frequency)),
                            0.01);
            }
        }
    }
}

// A low cutoff at a high sample rate (g ~ 3e-4): what remains of a low tone besides the analytic
// steady-state response is the float output's own rounding (about -154 dB). With float states
// it was -119 to -125 dB.
TEST(Butterworth, LowCutoffAtAHighRateStaysClean) {
    constexpr float k_rate = 192000.0f;
    constexpr float k_cutoff = 20.0f;
    constexpr double k_tone = 10.0;
    for (const auto response : {Butterworth::Response::LowPass, Butterworth::Response::HighPass}) {
        for (size_t order = 1; order <= Butterworth::k_max_order; ++order) {
            Butterworth filter;
            filter.prepare(k_cutoff, k_rate, order, response);
            // The analytic response: s = j tan(pi f / fs) / g through each section.
            const double g = Butterworth::prewarp(k_cutoff, k_rate);
            const std::complex<double> s(0.0, std::tan(std::numbers::pi * k_tone / k_rate) / g);
            std::complex<double> h = 1.0;
            const bool low = response == Butterworth::Response::LowPass;
            if (order % 2 == 1) { h *= low ? 1.0 / (s + 1.0) : s / (s + 1.0); }
            for (size_t k = 0; k < order / 2; ++k) {
                const double angle = std::numbers::pi * static_cast<double>(2 * k + 1 + order % 2) /
                                     static_cast<double>(2 * order);
                const std::complex<double> den = (s * s) + (2.0 * std::cos(angle) * s) + 1.0;
                h *= low ? 1.0 / den : (s * s) / den;
            }
            const size_t n = static_cast<size_t>(k_rate) * 6;
            const double w = 2.0 * std::numbers::pi * k_tone / k_rate;
            double residual = 0.0;
            double power = 0.0;
            for (size_t i = 0; i < n; ++i) {
                const auto x = static_cast<float>(0.5 * std::sin(w * static_cast<double>(i)));
                const float y = filter.process(x);
                if (i < n / 2) { continue; }
                const double expected =
                    0.5 * std::abs(h) * std::sin((w * static_cast<double>(i)) + std::arg(h));
                residual += (y - expected) * (y - expected);
                power += 0.125;
            }
            EXPECT_LT(10.0 * std::log10(residual / power), -140.0)
                << (low ? "low" : "high") << "-pass order " << order;
        }
    }
}
