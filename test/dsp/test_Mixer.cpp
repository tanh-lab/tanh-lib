#include <gtest/gtest.h>
#include <tanh/core/Buffer.h>
#include <tanh/core/BufferView.h>
#include <tanh/dsp/utils/Mixer.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <vector>

#include "TestSignals.h"

using thl::core::Buffer;
using thl::core::ConstBufferView;
using thl::dsp::utils::Mixer;

namespace {

constexpr double k_sample_rate = 48000.0;
constexpr size_t k_ramp_samples = 960;  // the default 20 ms at 48 kHz
constexpr size_t k_num_samples = 1024;  // past the ramp

Mixer prepared(size_t num_inputs) {
    Mixer mixer;
    mixer.prepare(k_sample_rate, num_inputs);
    return mixer;
}

/// Mixes constant inputs, one per value, into a two-channel output.
Buffer<float> mix_constants(Mixer& mixer, const std::vector<float>& values) {
    constexpr size_t k_length = 16;
    std::vector<Buffer<float>> buffers;
    for (const float value : values) { buffers.push_back(dsp_test::constant(2, k_length, value)); }
    const std::vector<ConstBufferView> inputs(buffers.begin(), buffers.end());
    Buffer<float> output(2, k_length);
    mixer.process(inputs, output);
    return output;
}

void expect_all(const Buffer<float>& signal, float value) {
    for (size_t ch = 0; ch < signal.get_num_channels(); ++ch) {
        for (size_t i = 0; i < signal.get_num_samples(); ++i) {
            EXPECT_FLOAT_EQ(signal.get_read_pointer(ch)[i], value)
                << "channel " << ch << " at " << i;
        }
    }
}

struct Gains {
    std::vector<float> m_first;
    std::vector<float> m_second;
};

/// The gains of a two-input mixer, sample by sample: input 0 is 1 in channel 0 and 0 in channel 1,
/// input 1 the other way round, so output channel c holds the gain of input c.
Gains measure_gains(Mixer& mixer, size_t num_samples = k_num_samples) {
    Buffer<float> first(2, num_samples);
    Buffer<float> second(2, num_samples);
    std::fill_n(first.get_write_pointer(0), num_samples, 1.0f);
    std::fill_n(second.get_write_pointer(1), num_samples, 1.0f);
    const std::array<ConstBufferView, 2> inputs{first, second};
    Buffer<float> output(2, num_samples);
    mixer.process(inputs, output);
    const float* gains_first = output.get_read_pointer(0);
    const float* gains_second = output.get_read_pointer(1);
    return {.m_first = {gains_first, gains_first + num_samples},
            .m_second = {gains_second, gains_second + num_samples}};
}

/// A settled two-input mixer in @p mode at @p p with weights @p first and @p second.
Mixer settled(Mixer::Mode mode, float p, float first, float second) {
    Mixer mixer = prepared(2);
    mixer.set_mode(mode);
    mixer.set_p(p);
    mixer.set_weights(std::array{first, second});
    mixer.reset();
    return mixer;
}

constexpr std::array k_norms{Mixer::k_amplitude,
                             Mixer::k_compromise,
                             Mixer::k_power,
                             Mixer::k_peak};

}  // namespace

TEST(Mixer, AnUnconfiguredMixerSums) {
    Mixer mixer = prepared(3);
    expect_all(mix_constants(mixer, {1.0f, 2.0f, 3.0f}), 6.0f);
}

TEST(Mixer, WithoutInputsTheOutputIsSilent) {
    Mixer mixer = prepared(0);
    Buffer<float> output = dsp_test::constant(2, 16, 1.0f);
    mixer.process({}, output);
    expect_all(output, 0.0f);
}

TEST(Mixer, SumScalesEachInputByItsWeight) {
    Mixer mixer = settled(Mixer::Mode::Sum, Mixer::k_amplitude, 0.5f, 2.0f);
    expect_all(mix_constants(mixer, {1.0f, 3.0f}), 6.5f);
}

TEST(Mixer, MixAtAmplitudeAverages) {
    Mixer mixer = prepared(4);
    mixer.set_mode(Mixer::Mode::Mix);
    mixer.reset();
    expect_all(mix_constants(mixer, {1.0f, 2.0f, 3.0f, 4.0f}), 2.5f);
}

TEST(Mixer, MixAtPowerKeepsTheSumOfSquaredGainsAtOne) {
    Mixer mixer = settled(Mixer::Mode::Mix, Mixer::k_power, 3.0f, 4.0f);
    const Gains gains = measure_gains(mixer, 16);
    EXPECT_NEAR(gains.m_first.back(), 0.6f, 1e-6f);
    EXPECT_NEAR(gains.m_second.back(), 0.8f, 1e-6f);
}

TEST(Mixer, MixAtCompromiseIsMinus4Point5DbForEqualWeights) {
    Mixer mixer = settled(Mixer::Mode::Mix, Mixer::k_compromise, 1.0f, 1.0f);
    const Gains gains = measure_gains(mixer, 16);
    EXPECT_NEAR(gains.m_first.back(), std::pow(2.0f, -0.75f), 1e-6f);
    EXPECT_NEAR(dsp_test::db(gains.m_second.back()), -4.52, 0.01);
}

TEST(Mixer, MixAtPeakKeepsTheLargestGainAtOne) {
    Mixer mixer = settled(Mixer::Mode::Mix, Mixer::k_peak, 2.0f, 1.0f);
    const Gains gains = measure_gains(mixer, 16);
    EXPECT_EQ(gains.m_first.back(), 1.0f);
    EXPECT_EQ(gains.m_second.back(), 0.5f);
}

TEST(Mixer, InMixOnlyTheRatiosOfTheWeightsMatter) {
    for (const float p : k_norms) {
        SCOPED_TRACE(testing::Message() << "p " << p);
        Mixer small = settled(Mixer::Mode::Mix, p, 1.0f, 3.0f);
        Mixer large = settled(Mixer::Mode::Mix, p, 2.0f, 6.0f);
        const Gains gains_small = measure_gains(small, 16);
        const Gains gains_large = measure_gains(large, 16);
        EXPECT_FLOAT_EQ(gains_small.m_first.back(), gains_large.m_first.back());
        EXPECT_FLOAT_EQ(gains_small.m_second.back(), gains_large.m_second.back());
    }
}

TEST(Mixer, ASingleWeightedInputPassesExactly) {
    for (const float p : k_norms) {
        for (const float weight : {1.0f, 0.3f, 7.0f}) {
            SCOPED_TRACE(testing::Message() << "p " << p << ", weight " << weight);
            Mixer mixer = settled(Mixer::Mode::Mix, p, weight, 0.0f);
            const Gains gains = measure_gains(mixer, 16);
            EXPECT_EQ(gains.m_first.back(), 1.0f);
            EXPECT_EQ(gains.m_second.back(), 0.0f);
            EXPECT_TRUE(mixer.passes_only(0));
        }
    }
}

TEST(Mixer, InMixAllWeightsZeroGiveSilence) {
    Mixer mixer = settled(Mixer::Mode::Mix, Mixer::k_power, 0.0f, 0.0f);
    expect_all(mix_constants(mixer, {1.0f, 1.0f}), 0.0f);
}

TEST(Mixer, InSumWeightsGlideLinearly) {
    Mixer mixer = settled(Mixer::Mode::Sum, Mixer::k_amplitude, 0.0f, 0.0f);
    mixer.set_weight(0, 1.0f);
    const Gains gains = measure_gains(mixer);
    for (size_t i = 0; i < k_ramp_samples; ++i) {
        EXPECT_NEAR(gains.m_first[i],
                    static_cast<float>(i + 1) / static_cast<float>(k_ramp_samples),
                    1e-4f)
            << "at " << i;
    }
    EXPECT_EQ(gains.m_first.back(), 1.0f);
}

TEST(Mixer, InMixGlidingWeightsKeepTheLevel) {
    for (const float p : {Mixer::k_amplitude, Mixer::k_power}) {
        SCOPED_TRACE(testing::Message() << "p " << p);
        Mixer mixer = settled(Mixer::Mode::Mix, p, 1.0f, 0.0f);
        mixer.set_weights(std::array{0.0f, 1.0f});
        const Gains gains = measure_gains(mixer);
        EXPECT_LT(gains.m_first.front(), 1.0f);  // glides, does not jump
        EXPECT_GT(gains.m_first.front(), 0.99f);
        for (size_t i = 0; i < k_num_samples; ++i) {
            const float level = std::pow(gains.m_first[i], p) + std::pow(gains.m_second[i], p);
            EXPECT_NEAR(level, 1.0f, 1e-5f) << "at " << i;
        }
        EXPECT_EQ(gains.m_first.back(), 0.0f);
        EXPECT_EQ(gains.m_second.back(), 1.0f);
    }
}

TEST(Mixer, ChangingTheModeCrossfades) {
    Mixer mixer = settled(Mixer::Mode::Sum, Mixer::k_amplitude, 1.0f, 1.0f);
    mixer.set_mode(Mixer::Mode::Mix);
    const Gains gains = measure_gains(mixer);
    for (size_t i = 0; i < k_ramp_samples; ++i) {
        const float amount = static_cast<float>(i + 1) / static_cast<float>(k_ramp_samples);
        EXPECT_NEAR(gains.m_first[i], 1.0f - (0.5f * amount), 1e-4f) << "at " << i;
    }
    EXPECT_EQ(gains.m_first.back(), 0.5f);
    EXPECT_EQ(gains.m_second.back(), 0.5f);
}

TEST(Mixer, ChangingPGlides) {
    Mixer mixer = settled(Mixer::Mode::Mix, Mixer::k_amplitude, 1.0f, 1.0f);
    for (const float p : {Mixer::k_power, Mixer::k_peak}) {
        SCOPED_TRACE(testing::Message() << "to p " << p);
        const float before = measure_gains(mixer, 1).m_first.back();
        mixer.set_p(p);
        const Gains gains = measure_gains(mixer);
        EXPECT_GT(gains.m_first.front(), before);
        EXPECT_LT(gains.m_first.front() - before, 0.01f);  // glides, does not jump
        for (size_t i = 1; i < k_num_samples; ++i) {
            EXPECT_GE(gains.m_first[i], gains.m_first[i - 1]) << "at " << i;
        }
        const float expected = p == Mixer::k_peak ? 1.0f : std::numbers::sqrt2_v<float> / 2.0f;
        EXPECT_FLOAT_EQ(gains.m_first.back(), expected);
        EXPECT_EQ(gains.m_first.back(), gains.m_second.back());
    }
}

// In Sum p does not matter: a p glide leaves the gains at the weights.
TEST(Mixer, InSumAPGlideChangesNothing) {
    Mixer mixer = settled(Mixer::Mode::Sum, Mixer::k_amplitude, 0.5f, 0.25f);
    mixer.set_p(Mixer::k_power);
    const Gains gains = measure_gains(mixer);
    for (size_t i = 0; i < gains.m_first.size(); ++i) {
        ASSERT_EQ(gains.m_first[i], 0.5f) << i;
        ASSERT_EQ(gains.m_second[i], 0.25f) << i;
    }
}

// All weights 0 stay silent through a change of mode (no 0 / 0).
TEST(Mixer, AllWeightsZeroStaySilentThroughAModeChange) {
    for (const auto mode : {Mixer::Mode::Mix, Mixer::Mode::Sum}) {
        Mixer mixer = settled(mode == Mixer::Mode::Mix ? Mixer::Mode::Sum : Mixer::Mode::Mix,
                              Mixer::k_power,
                              0.0f,
                              0.0f);
        mixer.set_mode(mode);
        const Gains gains = measure_gains(mixer);
        for (size_t i = 0; i < gains.m_first.size(); ++i) {
            ASSERT_EQ(gains.m_first[i], 0.0f) << i;
            ASSERT_EQ(gains.m_second[i], 0.0f) << i;
        }
    }
}

TEST(Mixer, ProcessingInBlocksGivesTheSameGains) {
    Mixer whole = settled(Mixer::Mode::Sum, Mixer::k_amplitude, 1.0f, 0.0f);
    Mixer blocks = settled(Mixer::Mode::Sum, Mixer::k_amplitude, 1.0f, 0.0f);
    for (Mixer* mixer : {&whole, &blocks}) {
        mixer->set_mode(Mixer::Mode::Mix);
        mixer->set_p(Mixer::k_power);
        mixer->set_weights(std::array{0.2f, 1.0f});
    }
    const Gains expected = measure_gains(whole);
    std::vector<float> first;
    for (size_t start = 0; start < k_num_samples; start += 64) {
        const Gains block = measure_gains(blocks, 64);
        first.insert(first.end(), block.m_first.begin(), block.m_first.end());
    }
    EXPECT_EQ(first, expected.m_first);
}

TEST(Mixer, TheOutputMayBeOneOfTheInputs) {
    const Buffer<float> first = dsp_test::noise(2, k_num_samples, 1.0f, 1);
    const Buffer<float> second = dsp_test::noise(2, k_num_samples, 1.0f, 2);
    Mixer apart = settled(Mixer::Mode::Mix, Mixer::k_power, 1.0f, 0.0f);
    Mixer in_place = settled(Mixer::Mode::Mix, Mixer::k_power, 1.0f, 0.0f);
    for (Mixer* mixer : {&apart, &in_place}) { mixer->set_weights(std::array{0.5f, 0.5f}); }

    Buffer<float> output(2, k_num_samples);
    apart.process(std::array<ConstBufferView, 2>{first, second}, output);

    Buffer<float> wet = dsp_test::noise(2, k_num_samples, 1.0f, 2);
    in_place.process(std::array<ConstBufferView, 2>{first, wet}, wet);

    for (size_t ch = 0; ch < 2; ++ch) {
        for (size_t i = 0; i < k_num_samples; ++i) {
            EXPECT_EQ(wet.get_read_pointer(ch)[i], output.get_read_pointer(ch)[i])
                << "channel " << ch << " at " << i;
        }
    }
}

// Settled too, the output may be one of the inputs, also one that is not the first.
TEST(Mixer, TheOutputMayBeOneOfTheInputsWhenSettled) {
    const Buffer<float> first = dsp_test::noise(2, k_num_samples, 1.0f, 1);
    const Buffer<float> second = dsp_test::noise(2, k_num_samples, 1.0f, 2);
    Mixer apart = settled(Mixer::Mode::Mix, Mixer::k_power, 0.5f, 0.25f);
    Mixer in_place = settled(Mixer::Mode::Mix, Mixer::k_power, 0.5f, 0.25f);

    Buffer<float> output(2, k_num_samples);
    apart.process(std::array<ConstBufferView, 2>{first, second}, output);
    Buffer<float> wet = dsp_test::noise(2, k_num_samples, 1.0f, 2);
    in_place.process(std::array<ConstBufferView, 2>{first, wet}, wet);

    for (size_t ch = 0; ch < 2; ++ch) {
        for (size_t i = 0; i < k_num_samples; ++i) {
            EXPECT_NEAR(wet.get_read_pointer(ch)[i], output.get_read_pointer(ch)[i], 1e-6f)
                << "channel " << ch << " at " << i;
        }
    }
}

TEST(Mixer, PassesOnlyWhenSettledAtOneInput) {
    Mixer mixer = settled(Mixer::Mode::Sum, Mixer::k_amplitude, 1.0f, 0.0f);
    EXPECT_TRUE(mixer.passes_only(0));
    EXPECT_FALSE(mixer.passes_only(1));
    EXPECT_FALSE(mixer.passes_only(2));

    mixer.set_weight(0, 0.5f);
    EXPECT_FALSE(mixer.passes_only(0));  // gliding
    mixer.reset();
    EXPECT_FALSE(mixer.passes_only(0));  // in Sum the gain is the weight

    mixer.set_mode(Mixer::Mode::Mix);
    mixer.reset();
    EXPECT_TRUE(mixer.passes_only(0));  // in Mix it normalises to 1

    mixer.set_weight(1, 0.1f);
    mixer.reset();
    EXPECT_FALSE(mixer.passes_only(0));
}

// A glide that cannot change a mix of one input does not keep it from passing: p never matters
// for one input, the mode not where its weight is 1. The mix it passes is that input exactly.
TEST(Mixer, PassesOnlyThroughGlidesThatChangeNothing) {
    Mixer mixer = settled(Mixer::Mode::Mix, Mixer::k_amplitude, 0.5f, 0.0f);
    mixer.set_p(Mixer::k_power);
    EXPECT_TRUE(mixer.passes_only(0));  // p glides: one input stays at gain 1
    mixer.set_mode(Mixer::Mode::Sum);
    EXPECT_FALSE(mixer.passes_only(0));  // the gain glides from 1 to the weight 0.5

    mixer = settled(Mixer::Mode::Sum, Mixer::k_amplitude, 1.0f, 0.0f);
    mixer.set_mode(Mixer::Mode::Mix);
    mixer.set_p(Mixer::k_peak);
    EXPECT_TRUE(mixer.passes_only(0));  // weight 1: gain 1 in both modes, at any p
    const auto gains = measure_gains(mixer);
    for (size_t i = 0; i < gains.m_first.size(); ++i) {
        ASSERT_EQ(gains.m_first[i], 1.0f) << i;
        ASSERT_EQ(gains.m_second[i], 0.0f) << i;
    }

    mixer.set_weight(1, 0.25f);
    EXPECT_FALSE(mixer.passes_only(0));  // the other input glides in
}

// A ramp shorter than a sample, 0 included, glides over one sample.
TEST(Mixer, ARampOfZeroGlidesOverOneSample) {
    Mixer mixer;
    mixer.prepare(k_sample_rate, 2, 0.0);
    mixer.set_weight(0, 0.0f);
    const auto gains = measure_gains(mixer, 2);
    EXPECT_EQ(gains.m_first[0], 0.0f);
    EXPECT_EQ(gains.m_first[1], 0.0f);
}

// Settled, the mix adds the inputs one after another (skipping the silent ones).
TEST(Mixer, ASettledMixAddsTheInputs) {
    Mixer mixer = prepared(3);
    mixer.set_mode(Mixer::Mode::Mix);
    mixer.set_weights(std::array{1.0f, 0.5f, 0.0f});
    mixer.reset();
    expect_all(mix_constants(mixer, {0.25f, -0.75f, 3.0f}),
               (0.25f * 1.0f / 1.5f) + (-0.75f * 0.5f / 1.5f));
}

TEST(Mixer, PrepareKeepsTheWeightsAndEndsTheGlides) {
    Mixer mixer = prepared(2);
    mixer.set_weight(1, 0.25f);
    mixer.prepare(44100.0, 3);
    EXPECT_EQ(mixer.num_inputs(), 3u);
    EXPECT_EQ(mixer.get_weight(0), 1.0f);
    EXPECT_EQ(mixer.get_weight(1), 0.25f);
    EXPECT_EQ(mixer.get_weight(2), 1.0f);
    expect_all(mix_constants(mixer, {1.0f, 1.0f, 1.0f}), 2.25f);
}

// prepare() with fewer inputs keeps the weights of those that remain.
TEST(Mixer, PrepareWithFewerInputsKeepsTheirWeights) {
    Mixer mixer = prepared(3);
    mixer.set_weights(std::array{0.5f, 0.25f, 2.0f});
    mixer.prepare(k_sample_rate, 2);
    EXPECT_EQ(mixer.num_inputs(), 2u);
    EXPECT_EQ(mixer.get_weight(0), 0.5f);
    EXPECT_EQ(mixer.get_weight(1), 0.25f);
    expect_all(mix_constants(mixer, {1.0f, 1.0f}), 0.75f);
}

TEST(Mixer, InvalidValuesAreClampedOrIgnored) {
    Mixer mixer = prepared(1);
    mixer.set_p(Mixer::k_power);
    mixer.set_p(std::numeric_limits<float>::quiet_NaN());
    EXPECT_EQ(mixer.get_p(), Mixer::k_power);
    mixer.set_p(0.5f);
    EXPECT_EQ(mixer.get_p(), 1.0f);
    mixer.set_p(Mixer::k_peak);
    EXPECT_EQ(mixer.get_p(), Mixer::k_peak);

    mixer.set_weight(0, -1.0f);
    EXPECT_EQ(mixer.get_weight(0), 0.0f);
    mixer.set_weight(0, 0.5f);
    mixer.set_weight(0, std::numeric_limits<float>::infinity());
    mixer.set_weight(0, std::numeric_limits<float>::quiet_NaN());
    EXPECT_EQ(mixer.get_weight(0), 0.5f);
}

#if !defined(NDEBUG) && GTEST_HAS_DEATH_TEST
TEST(MixerDeathTest, AssertsTheInputs) {
    Mixer mixer = prepared(2);
    const Buffer<float> input(2, 16);
    const Buffer<float> shorter(2, 8);
    Buffer<float> output(2, 16);

    // RTSan off for the call that asserts: its abort would be reported before the message.
    EXPECT_DEATH(
        {
            TANH_NONBLOCKING_SCOPED_DISABLER
            mixer.process(std::array<ConstBufferView, 1>{input}, output);
        },
        "one input per weight");
    EXPECT_DEATH(
        {
            TANH_NONBLOCKING_SCOPED_DISABLER
            mixer.process(std::array<ConstBufferView, 2>{input, shorter}, output);
        },
        "different dimensions");
}
#endif
