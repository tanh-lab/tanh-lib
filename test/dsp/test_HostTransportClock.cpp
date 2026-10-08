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
                   TransportInfo::k_has_time_signature | TransportInfo::k_is_playing;
    host.m_bpm = 98.0;
    host.m_beat_position = 12.5;
    host.m_sig_num = 6;
    host.m_sig_denom = 8;
    clk.set_host_info(host);
    clk.begin_block(256);
    EXPECT_TRUE(clk.is_playing());
    EXPECT_DOUBLE_EQ(clk.bpm(), 98.0);
    EXPECT_DOUBLE_EQ(clk.beat_at_sample(0), 12.5);
    EXPECT_NEAR(clk.beat_at_sample(256), 12.5 + 256 * bps(98.0), 1e-12);
    EXPECT_EQ(clk.sig_num(), 6);
    EXPECT_EQ(clk.sig_denom(), 8);
    EXPECT_EQ(clk.block_info().m_num_samples, 256u);
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
    TransportInfo host;  // a play flag without tempo, beat or time signature
    host.m_flags = TransportInfo::k_is_playing;
    auto info = run(clk, &host, 64);
    EXPECT_FALSE(info.is_playing());
    clk.play();
    info = run(clk, &host, 64);
    EXPECT_TRUE(info.is_playing());
    EXPECT_EQ(info.discontinuities(), TransportInfo::k_started);
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
            ASSERT_EQ(clk.block_info().discontinuities(),
                      b == 0 ? TransportInfo::k_timeline_reset : 0u);
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

TEST(HostTransportClock, SeekWhileHostSuppliesBeatWaitsForFreeRun) {
    HostTransportClock clk;
    clk.prepare(k_sr);
    TransportInfo host;
    host.m_flags = TransportInfo::k_has_tempo | TransportInfo::k_has_beat_position |
                   TransportInfo::k_is_playing;
    host.m_beat_position = 10.0;
    clk.set_position_beats(32.0);
    EXPECT_DOUBLE_EQ(run(clk, &host, 480).m_beat_position, 10.0);  // the host's beat wins

    TransportInfo tempo_only;
    tempo_only.m_flags = TransportInfo::k_has_tempo | TransportInfo::k_is_playing;
    const auto info = run(clk, &tempo_only, 480);
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

// A DAW reports one bpm per block (the tempo at the block start) but its ppq
// integrates the tempo map per sample, so a tempo step or ramp inside a block
// shows up as a start difference at the next block: a tempo change, never a jump.
namespace {

struct TempoMapRun {
    std::vector<uint32_t> m_flags;
    std::vector<double> m_start;       // resolved start beat
    std::vector<double> m_end;         // resolved end beat
    std::vector<double> m_host_start;  // the host's reported start beat
    std::vector<double> m_bpm;
};

TempoMapRun run_tempo_map(sim::SimHost& host, uint32_t block, uint64_t samples) {
    HostTransportClock clk;
    clk.prepare(k_sr);
    TempoMapRun r;
    while (host.now() < samples) {
        const auto h = host.next_block(block);
        const auto info = run(clk, &h, block);
        r.m_flags.push_back(info.discontinuities());
        r.m_start.push_back(info.m_beat_position);
        r.m_end.push_back(info.beat_end());
        r.m_host_start.push_back(h.m_beat_position);
        r.m_bpm.push_back(info.m_bpm);
    }
    return r;
}

// Every block after the first continues the previous one.
void expect_continuous(const TempoMapRun& r) {
    for (size_t b = 1; b < r.m_flags.size(); ++b) {
        ASSERT_EQ(r.m_flags[b] & TransportInfo::k_jumped, 0u) << "block " << b;
        ASSERT_NEAR(r.m_start[b], r.m_end[b - 1], 1e-9) << "block " << b;
    }
}

}  // namespace

class HostTransportClockTempoMap : public ::testing::TestWithParam<uint32_t> {};

INSTANTIATE_TEST_SUITE_P(Blocks,
                         HostTransportClockTempoMap,
                         ::testing::Values(64u, 256u, 1024u, 2048u),
                         [](const auto& info) { return "Block" + std::to_string(info.param); });

// Steps up and down, one sample into a block and in the middle of one: the
// block that reports the new tempo gets k_tempo_changed only, is re-sloped onto
// the host's next start, and from the block after it the clock is the host.
TEST_P(HostTransportClockTempoMap, NoJumpOnMidBlockTempoStep) {
    const uint32_t block = GetParam();
    for (const double to : {180.0, 60.0}) {
        for (const uint32_t into : {1u, block / 2}) {
            SCOPED_TRACE(testing::Message() << "to " << to << " bpm, " << into << " into");
            sim::SimHost host(k_sr, 120.0);
            const uint64_t at = (((96000 + block - 1) / block) * block) + into;
            host.add({.m_sample = 0, .m_kind = sim::Event::Kind::Play});
            host.add({.m_sample = at, .m_kind = sim::Event::Kind::TempoAt, .m_a = to});
            const TempoMapRun r = run_tempo_map(host, block, 192000);
            expect_continuous(r);
            const size_t changed = (at / block) + 1;  // first block that reports `to`
            for (size_t b = 1; b < r.m_flags.size(); ++b) {
                const uint32_t want = b == changed ? TransportInfo::k_tempo_changed : 0u;
                ASSERT_EQ(r.m_flags[b], want) << "block " << b;
            }
            EXPECT_NEAR(r.m_end[changed], r.m_host_start[changed + 1], 1e-9);
            for (size_t b = changed + 1; b < r.m_flags.size(); ++b) {
                ASSERT_NEAR(r.m_start[b], r.m_host_start[b], 1e-9) << "block " << b;
            }
        }
    }
}

// Linear ramps over 2 s, up and down: k_tempo_changed on every ramp block, no
// jump, and the clock trails the host by at most one block of the per-block
// tempo difference. A DAW loop wrap during the ramp still jumps.
TEST_P(HostTransportClockTempoMap, NoJumpOnTempoRamp) {
    const uint32_t block = GetParam();
    for (const double to : {60.0, 180.0}) {
        SCOPED_TRACE(testing::Message() << "to " << to << " bpm");
        sim::SimHost host(k_sr, 120.0);
        host.add({.m_sample = 0, .m_kind = sim::Event::Kind::Play});
        host.add(
            {.m_sample = 72000, .m_kind = sim::Event::Kind::TempoRamp, .m_a = to, .m_b = 96000});
        const TempoMapRun r = run_tempo_map(host, block, 240000);
        expect_continuous(r);
        size_t tempo_blocks = 0;
        for (size_t b = 1; b < r.m_flags.size(); ++b) {
            const double bound =
                (block * std::abs(r.m_bpm[b] - r.m_bpm[b - 1]) / (60.0 * k_sr)) + 1e-9;
            ASSERT_LE(std::abs(r.m_start[b] - r.m_host_start[b]), bound) << "block " << b;
            if (r.m_flags[b] == TransportInfo::k_tempo_changed) { ++tempo_blocks; }
        }
        EXPECT_GE(tempo_blocks, (96000 / block) - 1);
        EXPECT_NEAR(r.m_start.back(), r.m_host_start.back(), 1e-9);  // ramp over: exact again
    }
    {
        // A 2-beat DAW loop during a ramp: every wrap is a jump, nothing else is.
        sim::SimHost host(k_sr, 120.0);
        host.add({.m_sample = 0, .m_kind = sim::Event::Kind::Play});
        host.add({.m_sample = 0, .m_kind = sim::Event::Kind::Loop, .m_a = 0.0, .m_b = 2.0});
        host.add(
            {.m_sample = 0, .m_kind = sim::Event::Kind::TempoRamp, .m_a = 60.0, .m_b = 192000});
        HostTransportClock clk;
        clk.prepare(k_sr);
        int jumps = 0;
        int wraps = 0;
        double prev_host = 0.0;
        while (host.now() < 192000) {
            const auto h = host.next_block(block);
            const auto info = run(clk, &h, block);
            if (h.m_beat_position < prev_host) { ++wraps; }
            prev_host = h.m_beat_position;
            if (info.has(TransportInfo::k_jumped)) {
                ++jumps;
                EXPECT_LT(info.m_jump_delta_beats, -1.9);
            }
        }
        EXPECT_GT(wraps, 0);
        EXPECT_EQ(jumps, wraps);
    }
}

// A seek in the block that also reports a tempo step is still a jump, in both
// directions, including one smaller than a block.
TEST_P(HostTransportClockTempoMap, SeekWithTempoStepStillJumps) {
    const uint32_t block = GetParam();
    for (const double shift : {0.05, -0.05, 3.0}) {
        SCOPED_TRACE(testing::Message() << "shift " << shift);
        sim::SimHost host(k_sr, 120.0);
        const uint64_t boundary = ((96000 + block - 1) / block) * block;
        host.add({.m_sample = 0, .m_kind = sim::Event::Kind::Play});
        host.add({.m_sample = boundary - (block / 2),
                  .m_kind = sim::Event::Kind::TempoAt,
                  .m_a = 125.0});
        host.add({.m_sample = boundary, .m_kind = sim::Event::Kind::Shift, .m_a = shift});
        const TempoMapRun r = run_tempo_map(host, block, 144000);
        const size_t b = boundary / block;
        EXPECT_TRUE((r.m_flags[b] & TransportInfo::k_jumped) != 0);
        EXPECT_TRUE((r.m_flags[b] & TransportInfo::k_tempo_changed) != 0);
        EXPECT_DOUBLE_EQ(r.m_start[b], r.m_host_start[b]);
        for (size_t k = 1; k < r.m_flags.size(); ++k) {
            if (k != b) { ASSERT_EQ(r.m_flags[k] & TransportInfo::k_jumped, 0u) << k; }
        }
    }
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
    sum += static_cast<double>(clk.block_info().discontinuities());
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
