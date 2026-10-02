#include <gtest/gtest.h>
#include <tanh/dsp/transport/InternalTransportClock.h>
#include <tanh/dsp/transport/TransportInfo.h>

#include <cmath>
#include <cstdint>
#include <random>

using thl::dsp::transport::ContinuityTracker;
using thl::dsp::transport::Division;
using thl::dsp::transport::InternalTransportClock;
using thl::dsp::transport::TransportInfo;

namespace {

constexpr double k_sr = 48000.0;
constexpr double k_bpm = 120.0;
constexpr double k_bps = k_bpm / (60.0 * k_sr);

TransportInfo raw(double beat, bool playing, double bpm = k_bpm) {
    TransportInfo info;
    info.m_flags = TransportInfo::k_has_tempo | TransportInfo::k_has_beat_position;
    if (playing) { info.m_flags |= TransportInfo::k_is_playing; }
    info.m_bpm = bpm;
    info.m_beat_position = beat;
    return info;
}

}  // namespace

// ── TransportInfo ─────────────────────────────────────────────────────────────

TEST(TransportInfo, PhaseIsNegativeSafe) {
    TransportInfo info;
    info.m_beat_position = -0.5;
    EXPECT_DOUBLE_EQ(info.phase(4.0), 3.5);
    info.m_beat_position = -4.0;
    EXPECT_DOUBLE_EQ(info.phase(4.0), 0.0);
    info.m_beat_position = 9.25;
    EXPECT_DOUBLE_EQ(info.phase(4.0), 1.25);
    info.m_beat_position = -1e-18;  // rounds to unit → must wrap to 0
    EXPECT_GE(info.phase(4.0), 0.0);
    EXPECT_LT(info.phase(4.0), 4.0);
    EXPECT_DOUBLE_EQ(info.phase(0.0), 0.0);
}

TEST(TransportInfo, BeatAtAndEnd) {
    TransportInfo info;
    info.m_beat_position = 2.0;
    info.m_beats_per_sample = 0.01;
    info.m_num_samples = 100;
    EXPECT_DOUBLE_EQ(info.beat_at(50), 2.5);
    EXPECT_DOUBLE_EQ(info.beat_end(), 3.0);
    EXPECT_DOUBLE_EQ(info.phase(1.0, 50), 0.5);
}

TEST(TransportInfo, DivisionBoundaryIsHalfOpenAndNegativeSafe) {
    using thl::dsp::transport::division_boundary_in;
    EXPECT_TRUE(division_boundary_in(0.0, 0.1, Division::Beat, 4, 4));   // boundary at start
    EXPECT_FALSE(division_boundary_in(0.9, 1.0, Division::Beat, 4, 4));  // at end → next block
    EXPECT_TRUE(division_boundary_in(-0.1, 0.1, Division::Bar, 4, 4));   // count-in crosses 0
    EXPECT_TRUE(division_boundary_in(-4.05, -3.95, Division::Bar, 4, 4));
    EXPECT_FALSE(division_boundary_in(1.0, 1.0, Division::Beat, 4, 4));  // empty
    EXPECT_FALSE(division_boundary_in(2.0, 1.0, Division::Beat, 4, 4));  // reversed
    EXPECT_TRUE(division_boundary_in(3.4, 3.6, Division::Bar, 7, 8));    // 7/8 bar = 3.5
}

// ── ContinuityTracker ─────────────────────────────────────────────────────────

TEST(ContinuityTracker, NoJumpOnFirstBlock_ButTimelineReset) {
    ContinuityTracker tracker;
    tracker.prepare(k_sr);
    auto info = raw(17.0, true);
    tracker.resolve(info, 17.0 + 512 * k_bps, 512);
    EXPECT_TRUE(info.has(TransportInfo::k_timeline_reset));
    EXPECT_FALSE(info.has(TransportInfo::k_jumped));
    EXPECT_FALSE(info.has(TransportInfo::k_started));
    EXPECT_EQ(info.m_num_samples, 512u);
    EXPECT_NEAR(info.m_beats_per_sample, k_bps, 1e-15);

    auto next = raw(17.0 + 512 * k_bps, true);
    tracker.resolve(next, 17.0 + 1024 * k_bps, 512);
    EXPECT_EQ(next.discontinuities(), 0u);
}

TEST(ContinuityTracker, AbsorbsJitterBelowTolerance) {
    ContinuityTracker tracker;
    tracker.prepare(k_sr);
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> noise(-1.0, 1.0);  // ±1 sample

    double last_beat = -1.0;
    for (int b = 0; b < 2000; ++b) {
        const double ideal = b * 256 * k_bps;
        const double jitter = noise(rng) * k_bps;
        auto info = raw(ideal + jitter, true);
        tracker.resolve(info, ideal + 256 * k_bps + jitter, 256);
        if (b > 0) {
            EXPECT_EQ(info.discontinuities(), 0u) << "block " << b;
            EXPECT_GT(info.m_beat_position, last_beat);  // monotonic
            EXPECT_GT(info.m_beats_per_sample, 0.0);
        }
        last_beat = info.m_beat_position;
    }
}

TEST(ContinuityTracker, FlagsAboveTolerance) {
    ContinuityTracker tracker;
    tracker.prepare(k_sr);
    auto a = raw(0.0, true);
    tracker.resolve(a, 512 * k_bps, 512);
    auto b = raw(512 * k_bps + 0.25, true);
    tracker.resolve(b, 1024 * k_bps + 0.25, 512);
    EXPECT_TRUE(b.has(TransportInfo::k_jumped));
    EXPECT_NEAR(b.m_jump_delta_beats, 0.25, 1e-12);
    EXPECT_NEAR(b.m_beat_position, 512 * k_bps + 0.25, 1e-12);
    EXPECT_NEAR(tracker.tolerance_beats(k_bpm), 2 * k_bps, 1e-15);
}

TEST(ContinuityTracker, StartedStoppedTempoChanged) {
    ContinuityTracker tracker;
    tracker.prepare(k_sr);
    auto a = raw(1.0, false);
    tracker.resolve(a, 1.0, 64);
    auto b = raw(1.0, true);
    tracker.resolve(b, 1.0 + 64 * k_bps, 64);
    EXPECT_EQ(b.discontinuities(), TransportInfo::k_started);
    auto c = raw(1.0 + 64 * k_bps, true, 140.0);
    tracker.resolve(c, 1.0 + 64 * k_bps + 64 * 140.0 / (60.0 * k_sr), 64);
    EXPECT_EQ(c.discontinuities(), TransportInfo::k_tempo_changed);
    auto d = raw(c.beat_end(), false, 140.0);
    tracker.resolve(d, c.beat_end(), 64);
    EXPECT_EQ(d.discontinuities(), TransportInfo::k_stopped);
    EXPECT_DOUBLE_EQ(d.m_beats_per_sample, 0.0);
}

TEST(ContinuityTracker, HoldingSourceStaysFlatUnderJitter) {
    ContinuityTracker tracker;
    tracker.prepare(k_sr);
    auto a = raw(3.0, false);
    tracker.resolve(a, 3.0, 64);
    auto b = raw(3.0 + 0.5 * k_bps, false);  // stopped host reporting a jittered ppq
    tracker.resolve(b, 3.0 + 0.5 * k_bps, 64);
    EXPECT_EQ(b.discontinuities(), 0u);
    EXPECT_DOUBLE_EQ(b.m_beat_position, 3.0);
    EXPECT_DOUBLE_EQ(b.m_beats_per_sample, 0.0);
}

// ── InternalTransportClock discontinuities ───────────────────────────────────

TEST(InternalTransportClockInfo, ReportsSeekStartStopAndReset) {
    InternalTransportClock clk;
    clk.prepare(k_sr);
    clk.begin_block(512);
    EXPECT_TRUE(clk.block_info().has(TransportInfo::k_timeline_reset));
    clk.end_block();

    clk.play();
    clk.begin_block(512);
    EXPECT_EQ(clk.discontinuities(), TransportInfo::k_started);
    clk.end_block();

    clk.begin_block(512);
    EXPECT_EQ(clk.discontinuities(), 0u);
    clk.end_block();

    clk.set_position_beats(8.0);
    clk.begin_block(512);
    EXPECT_EQ(clk.discontinuities(), TransportInfo::k_jumped);
    EXPECT_NEAR(clk.block_info().m_beat_position, 8.0, 1e-9);
    clk.end_block();

    clk.stop();
    clk.begin_block(512);
    EXPECT_EQ(clk.discontinuities(), TransportInfo::k_stopped);
    clk.end_block();

    clk.set_position_beats(2.0);  // seek while stopped is a jump too
    clk.begin_block(512);
    EXPECT_EQ(clk.discontinuities(), TransportInfo::k_jumped);
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

TEST(TransportClockDefaults, BlockInfoFromGetters) {
    // A clock that does not override block_info() still yields a usable snapshot.
    class Minimal final : public thl::dsp::transport::TransportClock {
    public:
        void prepare(double) override {}
        void begin_block(uint32_t, std::optional<int64_t>) override {}
        void end_block() override {}
        [[nodiscard]] double beat_at_sample(uint32_t o) const override { return 2.0 + o * 0.5; }
        [[nodiscard]] bool division_in_block(Division) const override { return false; }
        [[nodiscard]] bool is_playing() const override { return true; }
        [[nodiscard]] double bpm() const override { return 90.0; }
        [[nodiscard]] int sig_num() const override { return 3; }
        [[nodiscard]] int sig_denom() const override { return 4; }
        [[nodiscard]] uint64_t sample_position() const override { return 0; }
        void set_bpm(double) override {}
        void set_time_signature(int, int) override {}
        void play() override {}
        void stop() override {}
        void set_position_beats(double) override {}
    } clock;
    const auto info = clock.block_info();
    EXPECT_TRUE(info.is_playing());
    EXPECT_DOUBLE_EQ(info.m_bpm, 90.0);
    EXPECT_DOUBLE_EQ(info.m_beat_position, 2.0);
    EXPECT_DOUBLE_EQ(info.m_beats_per_sample, 0.5);
    EXPECT_DOUBLE_EQ(info.m_quantum, 3.0);
    EXPECT_EQ(clock.discontinuities(), 0u);
}
