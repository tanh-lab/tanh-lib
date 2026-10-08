#include <gtest/gtest.h>
#include <tanh/dsp/transport/LinkAudio.h>

#include <array>
#include <cstdint>

#include "detail/LinkAudioCodec.h"
#include "detail/LinkSharing.h"

using thl::dsp::transport::LinkAudioPacket;
namespace detail = thl::link::detail;

TEST(LinkAudioCodec, MakesPacketsFromReceivedBuffers) {
    std::array<int16_t, 8> samples{16384, -16384, 0, 32767, -32768, 1, 2, 3};
    LinkAudioPacket p;
    ASSERT_TRUE(detail::make_link_audio_packet(samples.data(), 2, 4, 1.0, 2.0, p));
    EXPECT_EQ(p.m_num_channels, 2u);
    EXPECT_EQ(p.m_num_frames, 4u);
    EXPECT_FLOAT_EQ(p.m_samples[0], 0.5f);
    EXPECT_FLOAT_EQ(p.m_samples[1], -0.5f);
    EXPECT_DOUBLE_EQ(p.m_end_beat, 2.0);
    EXPECT_FALSE(detail::make_link_audio_packet(samples.data(), 2, 0, 1.0, 2.0, p));
    EXPECT_EQ(detail::to_link_audio_sample(2.0f), 32767);
    EXPECT_EQ(detail::to_link_audio_sample(-0.5f), -16384);
}

TEST(LinkAudioCodec, SendShapeKeepsMonoOrStereo) {
    const auto stereo = detail::send_shape(6, 64, 48000);
    ASSERT_TRUE(stereo.has_value());
    EXPECT_EQ(stereo->m_channels, 2u);
    EXPECT_EQ(stereo->m_samples, 128u);
    EXPECT_FALSE(detail::send_shape(0, 64, 48000).has_value());
    EXPECT_FALSE(detail::send_shape(2, 0, 48000).has_value());
    EXPECT_FALSE(detail::send_shape(2, 64, 0).has_value());
}

TEST(LinkAudioCodec, InterleavesPlanarChannels) {
    const std::array<float, 2> left{0.5f, -1.0f};
    const std::array<float, 2> right{0.0f, 1.0f};
    const std::array<const float*, 2> channels{left.data(), right.data()};
    const auto shape = detail::send_shape(2, 2, 48000);
    ASSERT_TRUE(shape.has_value());
    std::array<int16_t, 4> out{};
    detail::interleave(channels.data(), *shape, out.data());
    EXPECT_EQ(out[0], 16384);  // 0.5 * 32767, rounded
    EXPECT_EQ(out[1], 0);
    EXPECT_EQ(out[2], -32767);
    EXPECT_EQ(out[3], 32767);
}

TEST(LinkAudioCodec, ACaptureIsTakenOnce) {
    detail::CaptureTracker captures;
    EXPECT_FALSE(captures.take());
    captures.captured();
    EXPECT_TRUE(captures.take());
    EXPECT_FALSE(captures.take());  // no clock this block: the send captures afresh
}
