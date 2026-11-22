#include <gtest/gtest.h>
#include <tanh/dsp/transport/InternalTransportClock.h>
#include <tanh/dsp/transport/TransportInfo.h>

#include <cstdint>
#include <random>
#include <utility>

using thl::dsp::transport::Division;
using thl::dsp::transport::InternalTransportClock;
using thl::dsp::transport::TransportInfo;

namespace {
constexpr double k_sr = 48000.0;
}  // namespace

TEST(InternalTransportClockInfo, ReportsSeekStartStopAndReset) {
    InternalTransportClock clk;
    clk.prepare(k_sr);
    clk.begin_block(512);
    EXPECT_TRUE(clk.block_info().has(TransportInfo::k_timeline_reset));
    clk.end_block();

    clk.play();
    clk.begin_block(512);
    EXPECT_EQ(clk.block_info().discontinuities(), TransportInfo::k_started);
    clk.end_block();

    clk.begin_block(512);
    EXPECT_EQ(clk.block_info().discontinuities(), 0u);
    clk.end_block();

    clk.set_position_beats(8.0);
    clk.begin_block(512);
    EXPECT_EQ(clk.block_info().discontinuities(), TransportInfo::k_jumped);
    EXPECT_NEAR(clk.block_info().m_beat_position, 8.0, 1e-9);
    clk.end_block();

    clk.stop();
    clk.begin_block(512);
    EXPECT_EQ(clk.block_info().discontinuities(), TransportInfo::k_stopped);
    clk.end_block();

    clk.set_position_beats(2.0);  // seek while stopped is a jump too
    clk.begin_block(512);
    EXPECT_EQ(clk.block_info().discontinuities(), TransportInfo::k_jumped);
    clk.end_block();
}

TEST(InternalTransportClockInfo, TempoChangeKeepsBeatContinuous) {
    InternalTransportClock clk;
    clk.prepare(k_sr);
    clk.play();
    double expected_next = 0.0;
    for (int b = 0; b < 100; ++b) {
        if (b == 40) { clk.set_bpm(173.0); }
        clk.begin_block(300);
        const auto info = clk.block_info();
        if (b > 0) {
            EXPECT_NEAR(clk.beat_at_sample(0), expected_next, 1e-9) << "block " << b;
            EXPECT_FALSE(info.has(TransportInfo::k_jumped));
            EXPECT_EQ(info.has(TransportInfo::k_tempo_changed), b == 40);
        }
        EXPECT_NEAR(info.beat_at(299), clk.beat_at_sample(299), 1e-12);
        expected_next = clk.beat_at_sample(300);
        clk.end_block();
    }
}

TEST(InternalTransportClockInfo, DivisionInBlockMatchesTransportInfo) {
    for (const auto& [num, den] : {std::pair{4, 4}, std::pair{6, 8}, std::pair{7, 8}}) {
        InternalTransportClock clk;
        clk.prepare(k_sr);
        clk.set_time_signature(num, den);
        clk.play();
        std::mt19937 rng(static_cast<unsigned>(num * 10 + den));
        std::uniform_int_distribution<uint32_t> frames(1, 2048);
        for (int b = 0; b < 3000; ++b) {
            const uint32_t n = frames(rng);
            clk.begin_block(n);
            const auto info = clk.block_info();
            for (int d = 0; d < static_cast<int>(Division::NumDivisions); ++d) {
                const auto div = static_cast<Division>(d);
                ASSERT_EQ(info.division_in_block(div), clk.division_in_block(div))
                    << num << "/" << den << " block " << b << " div " << d;
            }
            clk.end_block();
        }
    }
}
