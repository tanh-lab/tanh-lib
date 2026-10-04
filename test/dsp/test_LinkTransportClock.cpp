// LinkTransportClock over a scripted fake Link session. Runs without
// TANH_WITH_LINK; the real SDK backends are tested in test/link.

#include <gtest/gtest.h>
#include <tanh/dsp/transport/LinkTransportClock.h>
#include <tanh/dsp/transport/TransportInfo.h>

#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include "FakeLinkBackend.h"

using thl::dsp::transport::LinkTransportClock;
using thl::dsp::transport::TransportInfo;

namespace {

constexpr double k_sr = 48000.0;
constexpr uint32_t k_frames = 480;     // 10 ms
constexpr int64_t k_block_us = 10000;  // 480 / 48000 s

double beats(int64_t us, double bpm = 120.0) {
    return static_cast<double>(us) * bpm / 60e6;
}

}  // namespace

TEST(LinkTransportClock, FakeLink_DeterministicScript) {
    fake::FakeLinkBackend link;
    LinkTransportClock clk(link);
    clk.prepare(k_sr);

    std::vector<uint32_t> flags;
    double prev_end = 0.0;
    double tempo = 120.0;
    for (int b = 0; b < 100; ++b) {
        const int64_t t0 = b * k_block_us;
        // Peer actions arrive between blocks.
        if (b >= 10 && b < 20) {
            tempo += 1.0;
            link.peer_set_tempo(tempo, t0);  // a peer ramps the tempo
        }
        if (b == 30) { link.peer_join_realign(1.5, t0); }
        if (b == 40) { link.peer_start(42 * k_block_us + 3000); }  // starts inside block 42
        if (b == 60) { link.peer_stop(t0); }
        if (b == 70) {
            link.m_has_peers = false;  // peers left; a local start maps beat 0 exactly
            clk.play();
        }

        clk.begin_block(k_frames, t0);
        const TransportInfo info = clk.block_info();
        flags.push_back(info.discontinuities());

        if (b > 0 && !info.has(TransportInfo::k_jumped)) {
            ASSERT_NEAR(info.m_beat_position, prev_end, 1e-9) << "block " << b;
        }
        if (b == 30) { EXPECT_NEAR(info.m_jump_delta_beats, 1.5, 1e-9); }
        if (b == 40 || b == 41) {
            EXPECT_LT(info.m_beat_position, 0.0) << "count-in before the peer's start";
            EXPECT_FALSE(info.is_playing());
        }
        if (b == 42) {
            EXPECT_TRUE(info.is_playing());
            EXPECT_LT(info.m_beat_position, 0.0);
            EXPECT_NEAR(info.beat_at(144), 0.0, info.m_beats_per_sample);  // 3 ms = 144 samples
        }
        if (b == 70) {
            EXPECT_EQ(link.m_request_start_calls, 1);
            EXPECT_DOUBLE_EQ(info.m_beat_position, 0.0);  // beat 0 at the local play time
            EXPECT_TRUE(info.is_playing());
        }
        prev_end = info.beat_end();
        clk.end_block();
    }

    for (size_t b = 0; b < flags.size(); ++b) {
        uint32_t expected = 0;
        if (b == 0) { expected = TransportInfo::k_timeline_reset; }
        if (b >= 10 && b < 20) { expected = TransportInfo::k_tempo_changed; }
        if (b == 30) { expected = TransportInfo::k_jumped | TransportInfo::k_timeline_reset; }
        if (b == 40) { expected = TransportInfo::k_jumped; }  // the peer remapped beat 0
        if (b == 42) { expected = TransportInfo::k_started; }
        if (b == 60) { expected = TransportInfo::k_stopped; }
        if (b == 70) { expected = TransportInfo::k_started | TransportInfo::k_jumped; }
        EXPECT_EQ(flags[b], expected) << "block " << b;
    }
    EXPECT_EQ(link.m_set_tempo_calls, 0);  // never pushed a local tempo
}

// A peer's tempo change reaches us after the time it applies from: the session
// keeps the beat continuous at that time, which lies inside our previous block,
// so the new start is ahead of the end we predicted at the old tempo. Same
// contract as a host's in-block tempo change: k_tempo_changed, not k_jumped, and
// the slope lands on the session's timeline.
TEST(LinkTransportClock, PeerTempoChangeInsidePreviousBlockIsNotAJump) {
    fake::FakeLinkBackend link;
    LinkTransportClock clk(link);
    clk.prepare(k_sr);
    link.m_session.m_playing = true;
    double prev_end = 0.0;
    for (int b = 0; b < 40; ++b) {
        const int64_t t0 = b * k_block_us;
        if (b == 10) { link.peer_set_tempo(180.0, t0 - 4000); }  // 4 ms into block 9
        if (b == 20) { link.peer_set_tempo(90.0, t0 - 9000); }   // 1 ms into block 19
        clk.begin_block(k_frames, t0);
        const auto info = clk.block_info();
        if (b > 0) {
            const uint32_t want = (b == 10 || b == 20) ? TransportInfo::k_tempo_changed : 0u;
            ASSERT_EQ(info.discontinuities(), want) << "block " << b;
            ASSERT_NEAR(info.m_beat_position, prev_end, 1e-9) << "block " << b;
        }
        if (b == 10 || b == 20) {
            // Re-sloped onto the session timeline at the block end.
            EXPECT_NEAR(info.beat_end(), link.m_session.beat_at(t0 + k_block_us), 1e-9);
        }
        prev_end = info.beat_end();
        clk.end_block();
    }
}

TEST(LinkTransportClock, HostTimeJitterRaisesNoFlags) {
    fake::FakeLinkBackend link;
    LinkTransportClock clk(link);
    clk.prepare(k_sr);
    std::mt19937 rng(9);
    std::uniform_int_distribution<int64_t> jitter(-50, 50);
    double last = -1.0;
    for (int b = 0; b < 3000; ++b) {
        clk.begin_block(k_frames, (b * k_block_us) + jitter(rng));
        const auto info = clk.block_info();
        if (b > 0) {
            ASSERT_EQ(info.discontinuities(), 0u) << "block " << b;
            ASSERT_GT(info.m_beat_position, last);
            // Never drifts from the true timeline by more than one block's jitter.
            ASSERT_NEAR(info.m_beat_position, beats(b * k_block_us), beats(51));
        }
        last = info.m_beat_position;
        clk.end_block();
    }
}

TEST(LinkTransportClock, LatencyCompensation) {
    EXPECT_EQ(LinkTransportClock::latency_us(480, 48000.0), 10000);  // not 0 (µs, not s)
    EXPECT_EQ(LinkTransportClock::latency_us(256, 44100.0), 5805);
    EXPECT_EQ(LinkTransportClock::latency_us(0, 48000.0), 0);

    fake::FakeLinkBackend link;
    LinkTransportClock plain(link);
    LinkTransportClock compensated(link);
    plain.prepare(k_sr);
    compensated.prepare(k_sr);
    constexpr uint32_t k_latency = 480;
    compensated.set_output_latency_samples(k_latency);

    plain.begin_block(k_frames, 123456);
    compensated.begin_block(k_frames, 123456);
    const double bps = plain.block_info().m_beats_per_sample;
    EXPECT_NEAR(compensated.beat_at_sample(0) - plain.beat_at_sample(0), k_latency * bps, 1e-9);
    EXPECT_EQ(compensated.output_time_us() - plain.output_time_us(), 10000);
    EXPECT_EQ(compensated.block_info().m_host_time_ns, 123456000);  // callback time, no latency
}

TEST(LinkTransportClock, EnableDisable_NoJumpWithoutPeers) {
    fake::FakeLinkBackend link;
    LinkTransportClock clk(link);
    clk.prepare(k_sr);
    for (int b = 0; b < 20; ++b) {
        if (b == 5 || b == 12) { link.enable_without_peers(); }  // BEATTIME-1
        clk.begin_block(k_frames, b * k_block_us);
        if (b > 0) { EXPECT_EQ(clk.discontinuities(), 0u) << "block " << b; }
        clk.end_block();
    }
}

TEST(LinkTransportClock, NoLocalTempoPushOnEnable) {
    fake::FakeLinkBackend link;
    link.m_session.m_tempo = 97.0;  // session tempo set by someone else
    LinkTransportClock clk(link);
    clk.prepare(k_sr);
    for (int b = 0; b < 10; ++b) {
        if (b == 3) { link.peer_join_realign(0.0, b * k_block_us); }
        clk.begin_block(k_frames, b * k_block_us);
        EXPECT_DOUBLE_EQ(clk.bpm(), 97.0);
        clk.end_block();
    }
    EXPECT_EQ(link.m_set_tempo_calls, 0);
    EXPECT_EQ(link.m_commits, 0);

    clk.set_bpm(133.0);  // explicit user request: applied once
    clk.begin_block(k_frames, 10 * k_block_us);
    EXPECT_DOUBLE_EQ(clk.bpm(), 133.0);
    EXPECT_TRUE(clk.block_info().has(TransportInfo::k_tempo_changed));
    clk.end_block();
    clk.begin_block(k_frames, 11 * k_block_us);
    clk.end_block();
    EXPECT_EQ(link.m_set_tempo_calls, 1);
    EXPECT_EQ(link.m_commits, 1);
}

TEST(LinkTransportClock, LocalStopAndSeek) {
    fake::FakeLinkBackend link;
    LinkTransportClock clk(link);
    clk.prepare(k_sr);
    clk.play();
    clk.begin_block(k_frames, 0);
    clk.end_block();
    clk.begin_block(k_frames, k_block_us);
    EXPECT_TRUE(clk.is_playing());
    clk.end_block();

    clk.set_position_beats(8.0);
    clk.begin_block(k_frames, 2 * k_block_us);
    EXPECT_DOUBLE_EQ(clk.beat_at_sample(0), 8.0);
    EXPECT_TRUE(clk.block_info().has(TransportInfo::k_jumped));
    clk.end_block();

    clk.stop();
    clk.begin_block(k_frames, 3 * k_block_us);
    EXPECT_FALSE(clk.is_playing());
    EXPECT_EQ(clk.discontinuities(), TransportInfo::k_stopped);
    EXPECT_GT(clk.block_info().m_beats_per_sample, 0.0);  // Link's timeline keeps running
    EXPECT_FALSE(clk.division_in_block(thl::dsp::transport::Division::Sixteenth));
    clk.end_block();
}

TEST(LinkTransportClock, NoHostTimeUsesFilteredSampleClock) {
    fake::FakeLinkBackend link;
    LinkTransportClock clk(link);
    clk.prepare(k_sr);
    std::mt19937 rng(1);
    std::uniform_int_distribution<int64_t> wake(0, 2000);  // callback wake-up jitter
    for (int b = 0; b < 2000; ++b) {
        link.m_now = 5000000 + (b * k_block_us) + wake(rng);
        clk.begin_block(k_frames);
        if (b > 600) {  // filter settled
            ASSERT_EQ(clk.discontinuities(), 0u) << "block " << b;
            ASSERT_NEAR(clk.output_time_us(), 5001000 + (b * k_block_us), 300.0) << "block " << b;
        }
        clk.end_block();
    }
}

namespace {

double rt_link_block(LinkTransportClock& clk, int64_t t) TANH_NONBLOCKING_FUNCTION {
    clk.begin_block(k_frames, t);
    const double v = clk.beat_at_sample(0) + static_cast<double>(clk.discontinuities());
    clk.end_block();
    return v;
}

}  // namespace

TEST(LinkTransportClock, AudioPathIsRealtimeSafe_FakeBackend) {
    fake::FakeLinkBackend link;
    LinkTransportClock clk(link);
    clk.prepare(k_sr);
    double acc = 0.0;
    for (int b = 0; b < 100; ++b) {
        if (b == 10) { clk.play(); }
        if (b == 20) { clk.set_bpm(90.0); }
        acc += rt_link_block(clk, b * k_block_us);
    }
    EXPECT_GT(acc, 0.0);
}
