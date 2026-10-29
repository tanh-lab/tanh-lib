// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include <tanh/dsp/utils/Resampler.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

using thl::dsp::utils::Resampler;

std::vector<float> sine(double frequency, double rate, std::size_t count, float amplitude = 0.5f) {
    std::vector<float> out(count);
    for (std::size_t i = 0; i < count; ++i) {
        out[i] = amplitude * static_cast<float>(std::sin(2.0 * std::numbers::pi * frequency *
                                                         static_cast<double>(i) / rate));
    }
    return out;
}

/// Runs `in` through a prepared resampler in blocks of `block` samples.
std::vector<float> run(Resampler& resampler, const std::vector<float>& in, std::size_t block) {
    std::vector<float> out;
    std::vector<float> buffer(resampler.max_output(block));
    for (std::size_t start = 0; start < in.size(); start += block) {
        const auto count = std::min(block, in.size() - start);
        const auto written = resampler.process(std::span(in).subspan(start, count), buffer);
        out.insert(out.end(),
                   buffer.begin(),
                   buffer.begin() + static_cast<std::ptrdiff_t>(written));
    }
    return out;
}

double rms(std::span<const float> x) {
    double sum = 0.0;
    for (const float v : x) { sum += static_cast<double>(v) * v; }
    return x.empty() ? 0.0 : std::sqrt(sum / static_cast<double>(x.size()));
}

TEST(Resampler, SameRateIsACopy) {
    Resampler resampler;
    resampler.prepare(16000.0, 16000.0, 64);
    EXPECT_TRUE(resampler.is_passthrough());
    const auto in = sine(440.0, 16000.0, 64);
    EXPECT_EQ(run(resampler, in, 64), in);
}

// Down and up: the rates a 16 kHz speech model and a 44.1 kHz effect model meet.
constexpr std::array<std::pair<double, double>, 6> k_conversions{{{48000.0, 16000.0},
                                                                  {44100.0, 16000.0},
                                                                  {96000.0, 16000.0},
                                                                  {8000.0, 16000.0},
                                                                  {16000.0, 48000.0},
                                                                  {44100.0, 48000.0}}};

TEST(Resampler, OutputCountFollowsTheRatio) {
    for (const auto& [from, to] : k_conversions) {
        Resampler resampler;
        resampler.prepare(from, to, 512);
        const auto out = run(resampler, sine(300.0, from, static_cast<std::size_t>(from)), 512);
        // Minus the filter's half length (25.3 samples at the lower rate) and its look-ahead.
        EXPECT_NEAR(static_cast<double>(out.size()), to, 27.0 * std::max(1.0, to / from) + 2.0)
            << from << " -> " << to;
        EXPECT_LE(out.size(), static_cast<std::size_t>(to) + 2) << from << " -> " << to;
    }
}

TEST(Resampler, PassbandKeepsLevel) {
    for (const auto& [from, to] : k_conversions) {
        Resampler resampler;
        resampler.prepare(from, to, 480);
        const auto out = run(resampler, sine(1000.0, from, static_cast<std::size_t>(from)), 480);
        const auto steady = std::span(out).subspan(static_cast<std::size_t>(to / 16.0),
                                                   static_cast<std::size_t>(to / 2.0));
        EXPECT_NEAR(rms(steady), 0.5 / std::numbers::sqrt2, 0.005) << from << " -> " << to;
    }
}

TEST(Resampler, DcGainIsOne) {
    for (const auto& [from, to] : k_conversions) {
        Resampler resampler;
        resampler.prepare(from, to, 256);
        const auto out =
            run(resampler, std::vector<float>(static_cast<std::size_t>(from), 0.25f), 256);
        for (std::size_t i = 200; i < out.size(); ++i) {
            ASSERT_NEAR(out[i], 0.25f, 1e-3f) << from << " -> " << to << ", sample " << i;
        }
    }
}

TEST(Resampler, DownAndUpAgainKeepsAPassbandSine) {
    // 48 -> 44.1 -> 48 kHz, as an effect at its model's rate runs: the round trip is the sine
    // again, delayed by both filters (found by correlation), within the filters' ripple.
    Resampler down;
    Resampler up;
    down.prepare(48000.0, 44100.0, 480);
    up.prepare(44100.0, 48000.0, down.max_output(480));
    const auto in = sine(440.0, 48000.0, 48000);
    const auto middle = run(down, in, 480);
    const auto out = run(up, middle, down.max_output(480));
    std::size_t best_delay = 0;
    double best_error = 1e9;
    for (std::size_t delay = 0; delay < 400; ++delay) {
        double error = 0.0;
        for (std::size_t i = 10000; i < 30000; ++i) {
            error = std::max(error, static_cast<double>(std::abs(out[i] - in[i - delay])));
        }
        if (error < best_error) {
            best_error = error;
            best_delay = delay;
        }
    }
    EXPECT_LT(best_error, 2e-3) << "delay " << best_delay;
}

TEST(Resampler, RejectsAboveTheNewNyquist) {
    Resampler resampler;
    resampler.prepare(48000.0, 16000.0, 480);
    // 12 kHz would alias to 4 kHz at 16 kHz; the filter must remove it (>= 50 dB).
    const auto out = run(resampler, sine(12000.0, 48000.0, 48000), 480);
    const auto level = rms(std::span(out).subspan(1000, 8000)) / (0.5 / std::numbers::sqrt2);
    EXPECT_LT(20.0 * std::log10(level), -50.0);
}

TEST(Resampler, BlockSizeDoesNotChangeTheResult) {
    const auto in = sine(700.0, 48000.0, 20000);
    Resampler whole;
    whole.prepare(48000.0, 16000.0, in.size());
    const auto reference = run(whole, in, in.size());

    Resampler pieces;
    pieces.prepare(48000.0, 16000.0, 1024);
    std::vector<float> out;
    std::vector<float> buffer(pieces.max_output(1024));
    std::size_t start = 0;
    for (std::size_t block = 7; start < in.size(); block = (block * 37) % 1000 + 1) {
        const auto count = std::min(block, in.size() - start);
        const auto written = pieces.process(std::span(in).subspan(start, count), buffer);
        out.insert(out.end(),
                   buffer.begin(),
                   buffer.begin() + static_cast<std::ptrdiff_t>(written));
        start += count;
    }
    ASSERT_EQ(out.size(), reference.size());
    for (std::size_t i = 0; i < out.size(); ++i) { ASSERT_NEAR(out[i], reference[i], 1e-5f) << i; }
}

TEST(Resampler, ResetForgetsHistory) {
    Resampler resampler;
    resampler.prepare(48000.0, 16000.0, 480);
    (void)run(resampler, std::vector<float>(4800, 1.0f), 480);
    resampler.reset();
    const auto out = run(resampler, std::vector<float>(4800, 0.0f), 480);
    for (const float v : out) { ASSERT_EQ(v, 0.0f); }
}

TEST(Resampler, OversizedBlockIsCutNotOverrun) {
    Resampler resampler;
    resampler.prepare(48000.0, 16000.0, 64);
    std::vector<float> buffer(resampler.max_output(64));
    const std::vector<float> in(10'000, 0.1f);
    EXPECT_LE(resampler.process(in, buffer), buffer.size());
}

TEST(Resampler, RejectsInvalidRates) {
    Resampler resampler;
    EXPECT_THROW(resampler.prepare(0.0, 16000.0, 64), std::invalid_argument);
    EXPECT_THROW(resampler.prepare(48000.0, -1.0, 64), std::invalid_argument);
}

}  // namespace
