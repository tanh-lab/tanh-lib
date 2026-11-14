#include <gtest/gtest.h>
#include <tanh/core/Buffer.h>
#include <tanh/core/BufferView.h>
#include <tanh/dsp/utils/Crossfade.h>

#include <array>
#include <cstddef>

using thl::core::BufferF;
using thl::core::BufferView;
using thl::dsp::utils::crossfade;
using thl::dsp::utils::crossfade_gains;
using thl::dsp::utils::CrossfadeShape;

TEST(Crossfade, EqualPowerGainsKeepPowerAndHitEndpoints) {
    for (const float position : {0.0f, 0.1f, 0.25f, 0.5f, 0.75f, 0.9f, 1.0f}) {
        const auto [from_gain, to_gain] = crossfade_gains(position, CrossfadeShape::EqualPower);
        EXPECT_NEAR(from_gain * from_gain + to_gain * to_gain, 1.0f, 1e-6f) << position;
    }

    const auto [start_from, start_to] = crossfade_gains(0.0f, CrossfadeShape::EqualPower);
    EXPECT_EQ(start_from, 1.0f);
    EXPECT_EQ(start_to, 0.0f);

    const auto [end_from, end_to] = crossfade_gains(1.0f, CrossfadeShape::EqualPower);
    EXPECT_NEAR(end_from, 0.0f, 1e-7f);
    EXPECT_EQ(end_to, 1.0f);
}

TEST(Crossfade, TwoChannelLinearIntoSubBlockWritesOnlyThatRange) {
    constexpr size_t k_num_channels = 2;
    constexpr size_t k_fade_length = 5;
    constexpr size_t k_offset = 3;
    constexpr size_t k_out_length = k_offset + k_fade_length + 2;
    constexpr float k_untouched = -7.0f;
    constexpr std::array<float, k_num_channels> k_from_level{1.0f, 2.0f};
    constexpr std::array<float, k_num_channels> k_to_level{-1.0f, 4.0f};

    BufferF from(k_num_channels, k_fade_length);
    BufferF to(k_num_channels, k_fade_length);
    BufferF out(k_num_channels, k_out_length);
    for (size_t channel = 0; channel < k_num_channels; ++channel) {
        for (size_t i = 0; i < k_fade_length; ++i) {
            from.set_sample(channel, i, k_from_level[channel]);
            to.set_sample(channel, i, k_to_level[channel]);
        }
        for (size_t i = 0; i < k_out_length; ++i) { out.set_sample(channel, i, k_untouched); }
    }

    crossfade(from, to, BufferView(out).sub_block(k_offset, k_fade_length), CrossfadeShape::Linear);

    for (size_t channel = 0; channel < k_num_channels; ++channel) {
        for (size_t i = 0; i < k_out_length; ++i) {
            const bool inside = i >= k_offset && i < k_offset + k_fade_length;
            float expected = k_untouched;
            if (inside) {
                const float position =
                    static_cast<float>(i - k_offset) / static_cast<float>(k_fade_length - 1);
                expected =
                    (1.0f - position) * k_from_level[channel] + position * k_to_level[channel];
            }
            EXPECT_FLOAT_EQ(out.get_sample(channel, i), expected)
                << "channel=" << channel << " i=" << i;
        }
    }
}
