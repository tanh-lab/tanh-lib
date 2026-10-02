#include <gtest/gtest.h>
#include <tanh/dsp/transport/HostTransportClock.h>
#include <tanh/dsp/transport/TransportInfo.h>

#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include "SimHost.h"

using thl::dsp::transport::Division;
using thl::dsp::transport::HostTransportClock;
using thl::dsp::transport::TransportInfo;

namespace {

constexpr double k_sr = 48000.0;

double bps(double bpm) {
    return bpm / (60.0 * k_sr);
}

// One host block through the clock; returns the clock's snapshot.
TransportInfo run(HostTransportClock& clk, const TransportInfo* host, uint32_t frames) {
    if (host != nullptr) { clk.set_host_info(*host); }
    clk.begin_block(frames);
    const TransportInfo info = clk.block_info();
    clk.end_block();
    return info;
}

}  // namespace

TEST(HostTransportClock, FollowsHostBeatAndTempo) {
    HostTransportClock clk;
    clk.prepare(k_sr);
    TransportInfo host;
    host.m_flags = TransportInfo::k_has_tempo | TransportInfo::k_has_beat_position |
                   TransportInfo::k_has_time_signature | TransportInfo::k_is_playing |
                   TransportInfo::k_has_bar_start;
    host.m_bpm = 98.0;
    host.m_beat_position = 12.5;
    host.m_sig_num = 6;
    host.m_sig_denom = 8;
    host.m_bar_start_beats = 12.0;
    clk.set_host_info(host);
    clk.begin_block(256);
    EXPECT_TRUE(clk.is_playing());
    EXPECT_DOUBLE_EQ(clk.bpm(), 98.0);
    EXPECT_DOUBLE_EQ(clk.beat_at_sample(0), 12.5);
    EXPECT_NEAR(clk.beat_at_sample(256), 12.5 + 256 * bps(98.0), 1e-12);
    EXPECT_EQ(clk.sig_num(), 6);
    EXPECT_EQ(clk.sig_denom(), 8);
    const auto info = clk.block_info();
    EXPECT_TRUE(info.has(TransportInfo::k_has_bar_start));
    EXPECT_DOUBLE_EQ(info.m_bar_start_beats, 12.0);
    EXPECT_DOUBLE_EQ(info.m_quantum, 3.0);
    EXPECT_EQ(info.m_num_samples, 256u);
    clk.end_block();
    EXPECT_EQ(clk.sample_position(), 256u);
}

TEST(HostTransportClock, MissingTempoUsesFallback) {
    HostTransportClock clk;
    clk.prepare(k_sr);
    clk.set_bpm(140.0);
    clk.set_time_signature(3, 4);
    TransportInfo host;
    host.m_flags = TransportInfo::k_has_beat_position | TransportInfo::k_is_playing;
    host.m_beat_position = 1.0;
    const auto info = run(clk, &host, 512);
    EXPECT_DOUBLE_EQ(info.m_bpm, 140.0);
    EXPECT_EQ(info.m_sig_num, 3);
    EXPECT_TRUE(info.is_playing());  // host reported a musical field → host play state
    EXPECT_NEAR(info.m_beats_per_sample, bps(140.0), 1e-15);
}

TEST(HostTransportClock, NoHostInfoUsesFallbackPlayState) {
    HostTransportClock clk;
    clk.prepare(k_sr);
    TransportInfo host;  // JUCE standalone: host time only, no musical fields
    host.m_flags = TransportInfo::k_has_host_time;
    host.m_host_time_ns = 123456789;
    auto info = run(clk, &host, 64);
    EXPECT_FALSE(info.is_playing());
    EXPECT_TRUE(info.has(TransportInfo::k_has_host_time));
    EXPECT_EQ(info.m_host_time_ns, 123456789);
    clk.play();
    info = run(clk, &host, 64);
    EXPECT_TRUE(info.is_playing());
    EXPECT_EQ(info.discontinuities(), TransportInfo::k_started);

    clk.begin_block(64, int64_t{1000});  // host time from begin_block (µs → ns)
    EXPECT_EQ(clk.block_info().m_host_time_ns, 1000000);
    clk.end_block();
}

TEST(HostTransportClock, MissingBeatFreeRuns) {
    HostTransportClock clk;
    clk.prepare(k_sr);
    clk.set_bpm(133.0);
    clk.play();
    uint64_t total = 0;
    std::mt19937 rng(3);
    std::uniform_int_distribution<uint32_t> frames(1, 1024);
    for (int b = 0; b < 1000000; ++b) {
        const uint32_t n = frames(rng);
        clk.begin_block(n);
        if (b % 1000 == 0 || b == 999999) {
            const double expected = static_cast<double>(total) * bps(133.0);
            ASSERT_NEAR(clk.beat_at_sample(0), expected, 1e-9 * std::max(1.0, expected))
                << "block " << b;
            ASSERT_EQ(clk.discontinuities(), b == 0 ? TransportInfo::k_timeline_reset : 0u);
        }
        clk.end_block();
        total += n;
    }
}

TEST(HostTransportClock, FreeRunContinuesFromLastHostBeatAndFollowsSeek) {
    HostTransportClock clk;
    clk.prepare(k_sr);
    TransportInfo host;
    host.m_flags = TransportInfo::k_has_tempo | TransportInfo::k_has_beat_position |
                   TransportInfo::k_is_playing;
    host.m_bpm = 120.0;
    host.m_beat_position = 10.0;
    run(clk, &host, 480);
    // Host stops reporting the beat (e.g. AUv3 without musical context): free-run.
    TransportInfo tempo_only;
    tempo_only.m_flags = TransportInfo::k_has_tempo | TransportInfo::k_is_playing;
    tempo_only.m_bpm = 120.0;
    auto info = run(clk, &tempo_only, 480);
    EXPECT_NEAR(info.m_beat_position, 10.0 + 480 * bps(120.0), 1e-12);
    EXPECT_EQ(info.discontinuities(), 0u);

    clk.set_position_beats(32.0);
    info = run(clk, &tempo_only, 480);
    EXPECT_DOUBLE_EQ(info.m_beat_position, 32.0);
    EXPECT_TRUE(info.has(TransportInfo::k_jumped));
}

TEST(HostTransportClock, StoppedBeatConstantInBlock) {
    HostTransportClock clk;
    clk.prepare(k_sr);
    TransportInfo host;
    host.m_flags = TransportInfo::k_has_tempo | TransportInfo::k_has_beat_position;
    host.m_bpm = 120.0;
    host.m_beat_position = 5.0;
    clk.set_host_info(host);
    clk.begin_block(512);
    EXPECT_FALSE(clk.is_playing());
    EXPECT_DOUBLE_EQ(clk.beat_at_sample(0), 5.0);
    EXPECT_DOUBLE_EQ(clk.beat_at_sample(511), 5.0);
    EXPECT_FALSE(clk.division_in_block(Division::Sixteenth));
    clk.end_block();
    EXPECT_EQ(clk.sample_position(), 0u);
}

TEST(HostTransportClock, NoJumpOnFirstBlock_ButTimelineReset) {
    HostTransportClock clk;
    clk.prepare(k_sr);
    sim::SimHost host(k_sr);
    host.add({.m_sample = 0, .m_kind = sim::Event::Kind::Seek, .m_a = 64.0});
    host.add({.m_sample = 0, .m_kind = sim::Event::Kind::Play});
    auto h = host.next_block(512);
    const auto info = run(clk, &h, 512);
    EXPECT_EQ(info.discontinuities(), TransportInfo::k_timeline_reset);

    clk.prepare(k_sr);  // prepare() again → reset again
    h = host.next_block(512);
    EXPECT_EQ(run(clk, &h, 512).discontinuities(), TransportInfo::k_timeline_reset);
}

// Deterministic script: DAW loop wrap, seek, tempo change, start/stop requested
// mid-block. Every flag must appear exactly in the block where the host first
// reports the change, and nowhere else.
TEST(HostTransportClock, SimulatedHostScript) {
    constexpr uint32_t k_block = 500;  // not a divisor of a beat (24000 samples)
    HostTransportClock clk;
    clk.prepare(k_sr);
    sim::SimHost host(k_sr, 120.0);
    using K = sim::Event::Kind;
    host.add({.m_sample = 250, .m_kind = K::Play});  // mid-block → visible in block 1
    host.add({.m_sample = 0, .m_kind = K::Loop, .m_a = 4.0, .m_b = 8.0});
    host.add({.m_sample = 300000, .m_kind = K::Tempo, .m_a = 150.0});  // block 600
    host.add({.m_sample = 400100, .m_kind = K::Seek, .m_a = 1.0});     // block 801
    host.add({.m_sample = 500000, .m_kind = K::Stop});                 // block 1000
    host.add({.m_sample = 520000, .m_kind = K::Seek, .m_a = 2.0});     // seek while stopped
    host.add({.m_sample = 540250, .m_kind = K::Play});                 // block 1081

    std::vector<uint32_t> flags;
    std::vector<double> deltas;
    double prev_end = 0.0;
    for (int b = 0; b < 1200; ++b) {
        const auto h = host.next_block(k_block);
        const auto info = run(clk, &h, k_block);
        flags.push_back(info.discontinuities());
        deltas.push_back(info.m_jump_delta_beats);
        if (b > 0 && !info.has(TransportInfo::k_jumped)) {
            ASSERT_NEAR(info.m_beat_position, prev_end, 1e-9) << "block " << b;
        }
        prev_end = info.beat_end();
    }

    // Loop wraps: 4 beats of 24000 samples at 120 bpm = 96000 samples per cycle.
    int wraps = 0;
    for (size_t b = 0; b < flags.size(); ++b) {
        const uint32_t f = flags[b];
        if (b == 0) {
            EXPECT_EQ(f, TransportInfo::k_timeline_reset);
        } else if (b == 1) {
            EXPECT_EQ(f, TransportInfo::k_started) << "mid-block play shows at the next boundary";
        } else if (b == 600) {
            EXPECT_EQ(f, TransportInfo::k_tempo_changed);
        } else if (b == 801) {
            EXPECT_EQ(f, TransportInfo::k_jumped);
        } else if (b == 1000) {
            EXPECT_EQ(f, TransportInfo::k_stopped);
        } else if (b == 1040) {
            EXPECT_EQ(f, TransportInfo::k_jumped) << "seek while stopped";
        } else if (b == 1081) {
            EXPECT_EQ(f, TransportInfo::k_started);
        } else if (f == TransportInfo::k_jumped) {
            ++wraps;
            EXPECT_LT(deltas[b], -3.9) << "loop wrap goes back ~4 beats, block " << b;
        } else {
            EXPECT_EQ(f, 0u) << "block " << b;
        }
    }
    EXPECT_EQ(wraps, 3);  // blocks ~385 and ~577 at 120 bpm, ~731 at 150 bpm
}

TEST(HostTransportClock, JitteryHostPpqIsAbsorbed) {
    HostTransportClock clk;
    clk.prepare(k_sr);
    std::mt19937 rng(11);
    std::uniform_real_distribution<double> noise(-1.0, 1.0);
    for (int b = 0; b < 5000; ++b) {
        TransportInfo host;
        host.m_flags = TransportInfo::k_has_tempo | TransportInfo::k_has_beat_position |
                       TransportInfo::k_is_playing;
        host.m_bpm = 120.0;
        host.m_beat_position = (b * 256 + noise(rng)) * bps(120.0);
        const auto info = run(clk, &host, 256);
        if (b > 0) { ASSERT_EQ(info.discontinuities(), 0u) << "block " << b; }
    }
}

namespace {

double rt_block(HostTransportClock& clk, const TransportInfo& host) TANH_NONBLOCKING_FUNCTION {
    clk.set_host_info(host);
    clk.begin_block(128, int64_t{42});
    double sum = clk.beat_at_sample(0) + clk.beat_at_sample(127);
    sum += clk.division_in_block(Division::Beat) ? 1.0 : 0.0;
    sum += static_cast<double>(clk.block_info().m_num_samples);
    sum += static_cast<double>(clk.discontinuities());
    clk.end_block();
    return sum;
}

}  // namespace

// Under -DTANH_WITH_RTSAN=ON this aborts on any allocation/lock in the audio path.
TEST(HostTransportClock, AudioPathIsRealtimeSafe) {
    HostTransportClock clk;
    clk.prepare(k_sr);
    TransportInfo host;
    host.m_flags = TransportInfo::k_has_tempo | TransportInfo::k_has_beat_position |
                   TransportInfo::k_is_playing;
    double acc = 0.0;
    for (int b = 0; b < 100; ++b) {
        host.m_beat_position = b * 128 * bps(120.0);
        acc += rt_block(clk, host);
    }
    EXPECT_GT(acc, 0.0);
}
