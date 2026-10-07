#include <gtest/gtest.h>
#include <tanh/dsp/transport/LinkAudio.h>

#include <array>
#include <cstdint>
#include <deque>
#include <vector>

using thl::dsp::transport::LinkAudioBackend;
using thl::dsp::transport::LinkAudioPacket;
using thl::dsp::transport::LinkAudioReceiver;

namespace {

constexpr double k_bpm = 120.0;
constexpr double k_rate = 48000.0;
constexpr double k_beats_per_frame = k_bpm / (60.0 * k_rate);

// Packets scripted by the test; send() is not used here.
class FakeSharing final : public LinkAudioBackend {
public:
    bool pop(LinkAudioPacket& out) override {
        if (m_packets.empty()) { return false; }
        out = m_packets.front();
        m_packets.pop_front();
        return true;
    }
    bool send(const float* const*, uint32_t, uint32_t, double, double, uint32_t) override {
        return false;
    }

    void push(const LinkAudioPacket& packet) { m_packets.push_back(packet); }

    /// A packet whose samples are their own beat (channel 1: minus the beat), so a
    /// rendered sample shows the beat it was read at.
    void add(double begin_beat, uint32_t frames, uint32_t channels, double beats_per_frame) {
        LinkAudioPacket p;
        p.m_num_frames = frames;
        p.m_num_channels = channels;
        p.m_begin_beat = begin_beat;
        p.m_end_beat = begin_beat + (frames * beats_per_frame);
        for (uint32_t f = 0; f < frames; ++f) {
            const auto beat = static_cast<float>(begin_beat + (f * beats_per_frame));
            for (uint32_t ch = 0; ch < channels; ++ch) {
                p.m_samples[(f * channels) + ch] = ch == 0 ? beat : -beat;
            }
        }
        m_packets.push_back(p);
    }

    /// Consecutive packets of @p frames from @p begin_beat, @p count of them.
    double add_run(double begin_beat,
                   int count,
                   uint32_t frames = 256,
                   uint32_t channels = 2,
                   double beats_per_frame = k_beats_per_frame) {
        for (int i = 0; i < count; ++i) {
            add(begin_beat, frames, channels, beats_per_frame);
            begin_beat += frames * beats_per_frame;
        }
        return begin_beat;
    }

private:
    std::deque<LinkAudioPacket> m_packets;
};

struct Block {
    explicit Block(uint32_t frames, uint32_t channels = 2)
        : m_data(channels, std::vector<float>(frames, 99.0f)), m_frames(frames) {
        for (auto& ch : m_data) { m_ptrs.push_back(ch.data()); }
    }
    float* const* ptrs() { return m_ptrs.data(); }
    std::vector<std::vector<float>> m_data;
    std::vector<float*> m_ptrs;
    uint32_t m_frames;
};

bool render(LinkAudioReceiver& r, FakeSharing& s, Block& b, double begin_beat) {
    return r.render(s,
                    b.ptrs(),
                    static_cast<uint32_t>(b.m_data.size()),
                    b.m_frames,
                    begin_beat,
                    k_beats_per_frame);
}

}  // namespace

TEST(LinkAudioReceiver, SilentUntilTheBufferedAudioCoversTheBlock) {
    LinkAudioReceiver r;
    FakeSharing s;
    Block b(128);
    EXPECT_FALSE(render(r, s, b, 1.0));
    for (const auto& ch : b.m_data) {
        for (float v : ch) { EXPECT_EQ(v, 0.0f); }
    }
    // Audio that starts after the block's first beat does not cover it either.
    s.add_run(1.01, 4);
    EXPECT_FALSE(render(r, s, b, 1.0));
}

TEST(LinkAudioReceiver, PlaysTheAudioOfTheRequestedBeats) {
    LinkAudioReceiver r;
    FakeSharing s;
    s.add_run(0.0, 40);  // 10240 frames, about 0.43 beats
    Block b(128);
    double beat = 0.05;
    for (int block = 0; block < 20; ++block) {
        ASSERT_TRUE(render(r, s, b, beat)) << "block " << block;
        for (uint32_t f = 0; f < b.m_frames; ++f) {
            const double expected = beat + (f * k_beats_per_frame);
            EXPECT_NEAR(b.m_data[0][f], expected, 1e-5) << "block " << block << " frame " << f;
            EXPECT_NEAR(b.m_data[1][f], -expected, 1e-5);
        }
        beat += b.m_frames * k_beats_per_frame;
    }
}

TEST(LinkAudioReceiver, DropsAudioOlderThanTheBlockWhenStarting) {
    LinkAudioReceiver r;
    FakeSharing s;
    s.add_run(0.0, 40);
    Block b(64);
    ASSERT_TRUE(render(r, s, b, 0.2));
    EXPECT_NEAR(b.m_data[0][0], 0.2, 1e-5);
    EXPECT_LT(r.num_buffered(), 40u);
}

TEST(LinkAudioReceiver, MonoGoesToEveryChannel) {
    LinkAudioReceiver r;
    FakeSharing s;
    s.add_run(0.0, 10, 256, 1);
    Block b(64, 2);
    ASSERT_TRUE(render(r, s, b, 0.01));
    for (uint32_t f = 0; f < b.m_frames; ++f) { EXPECT_EQ(b.m_data[0][f], b.m_data[1][f]); }
}

TEST(LinkAudioReceiver, FollowsASenderAtAnotherSampleRate) {
    // 44.1 kHz frames carry more beats each; the output at 48 kHz still shows its beats.
    LinkAudioReceiver r;
    FakeSharing s;
    constexpr double sender_beats_per_frame = k_bpm / (60.0 * 44100.0);
    s.add_run(0.0, 40, 256, 2, sender_beats_per_frame);
    Block b(128);
    double beat = 0.05;
    for (int block = 0; block < 10; ++block) {
        ASSERT_TRUE(render(r, s, b, beat));
        for (uint32_t f = 0; f < b.m_frames; ++f) {
            EXPECT_NEAR(b.m_data[0][f], beat + (f * k_beats_per_frame), 1e-5);
        }
        beat += b.m_frames * k_beats_per_frame;
    }
}

TEST(LinkAudioReceiver, AGapSilencesAndPlaybackRestartsWhenAudioReturns) {
    LinkAudioReceiver r;
    FakeSharing s;
    const double end = s.add_run(0.0, 4);  // about 0.043 beats
    Block b(128);
    double beat = 0.01;
    ASSERT_TRUE(render(r, s, b, beat));
    EXPECT_GT(r.margin_beats(), 0.0);
    EXPECT_EQ(r.num_underruns(), 0u);
    // Past what arrived: silence, counted as a dropout.
    beat = end + 0.01;
    EXPECT_FALSE(render(r, s, b, beat));
    EXPECT_EQ(b.m_data[0][0], 0.0f);
    EXPECT_EQ(r.num_underruns(), 1u);
    // The audio for later beats arrives: it plays from the right beat.
    s.add_run(end, 20);
    ASSERT_TRUE(render(r, s, b, beat));
    EXPECT_NEAR(b.m_data[0][0], beat, 1e-5);
}

TEST(LinkAudioReceiver, ATimelineJumpBackRestarts) {
    LinkAudioReceiver r;
    FakeSharing s;
    s.add_run(0.0, 40);
    Block b(128);
    ASSERT_TRUE(render(r, s, b, 0.2));
    // The local timeline jumped back before the buffered audio: silence, then it starts over.
    EXPECT_FALSE(render(r, s, b, 0.05));
    s.add_run(0.0, 40);
    EXPECT_TRUE(render(r, s, b, 0.05));
    EXPECT_NEAR(b.m_data[0][0], 0.05, 1e-5);
}

TEST(LinkAudioReceiver, ALostPacketIsSkippedNotADropout) {
    LinkAudioReceiver r;
    FakeSharing s;
    double beat = s.add_run(0.0, 10);
    beat += 256 * k_beats_per_frame;  // the network lost the next packet
    s.add_run(beat, 30);
    Block b(128);
    double at = 0.01;
    for (int block = 0; block < 60; ++block) {
        EXPECT_TRUE(render(r, s, b, at)) << "block " << block;
        at += b.m_frames * k_beats_per_frame;
    }
    EXPECT_EQ(r.num_underruns(), 0u);
}

TEST(LinkAudioReceiver, APacketOutOfOrderIsDropped) {
    LinkAudioReceiver r;
    FakeSharing s;
    constexpr double span = 256 * k_beats_per_frame;
    for (const int i : {0, 1, 3, 2, 4, 5, 6, 7, 8, 9, 10, 11}) {
        s.add(i * span, 256, 2, k_beats_per_frame);
    }
    Block b(128);
    double at = 0.001;
    for (int block = 0; block < 20; ++block) {
        EXPECT_TRUE(render(r, s, b, at)) << "block " << block;
        at += b.m_frames * k_beats_per_frame;
    }
    EXPECT_EQ(r.num_underruns(), 0u);
}

TEST(LinkAudioReceiver, IgnoresEmptyOrOversizedPackets) {
    LinkAudioReceiver r;
    FakeSharing s;
    s.add(0.0, 0, 2, k_beats_per_frame);
    LinkAudioPacket big;
    big.m_num_frames = 400;
    big.m_num_channels = 2;  // 800 samples > k_max_samples
    big.m_end_beat = 1.0;
    s.push(big);
    Block b(16);
    EXPECT_FALSE(render(r, s, b, 0.0));
    EXPECT_EQ(r.num_buffered(), 0u);
}
