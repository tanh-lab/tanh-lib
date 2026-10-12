#include <gtest/gtest.h>
#include <tanh/dsp/transport/ContinuityTracker.h>
#include <tanh/dsp/transport/TransportInfo.h>

#include <cmath>
#include <cstdint>
#include <random>

using thl::dsp::transport::ContinuityTracker;
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

// A tempo step 120 → 180 halfway through a 1024-sample block: the host reports
// 180 at the next block, whose start is ahead of the predicted end by half a
// block of the tempo difference. That is a tempo change, not a jump; the start
// snaps and the slope lands on the raw end.
TEST(ContinuityTracker, AbsorbsTempoChangeInsidePreviousBlock) {
    ContinuityTracker tracker;
    tracker.prepare(k_sr);
    const double fast = 180.0 / (60.0 * k_sr);
    auto a = raw(0.0, true);
    tracker.resolve(a, 1024 * k_bps, 1024);
    auto b = raw(1024 * k_bps, true);
    tracker.resolve(b, 2048 * k_bps, 1024);
    const double host = (2048 * k_bps) + (512 * (fast - k_bps));  // real position
    auto c = raw(host, true, 180.0);
    tracker.resolve(c, host + (1024 * fast), 1024);
    EXPECT_EQ(c.discontinuities(), TransportInfo::k_tempo_changed);
    EXPECT_DOUBLE_EQ(c.m_beat_position, b.beat_end());
    EXPECT_NEAR(c.beat_end(), host + (1024 * fast), 1e-12);  // lands on the host's end
    auto d = raw(host + (1024 * fast), true, 180.0);
    tracker.resolve(d, host + (2048 * fast), 1024);
    EXPECT_EQ(d.discontinuities(), 0u);
    EXPECT_DOUBLE_EQ(d.m_beat_position, host + (1024 * fast));
}

// The tempo window points in the direction of the change: a faster tempo can
// only put the host ahead of the prediction, never behind it. And it is no
// wider than one previous block of the tempo difference.
TEST(ContinuityTracker, TempoWindowIsSignedAndBounded) {
    const double fast = 180.0 / (60.0 * k_sr);
    const double e = 1024 * (fast - k_bps);
    auto third_block = [&](double delta) {
        ContinuityTracker tracker;
        tracker.prepare(k_sr);
        auto a = raw(0.0, true);
        tracker.resolve(a, 1024 * k_bps, 1024);
        auto b = raw(1024 * k_bps, true);
        tracker.resolve(b, 2048 * k_bps, 1024);
        auto c = raw((2048 * k_bps) + delta, true, 180.0);
        tracker.resolve(c, (2048 * k_bps) + delta + (1024 * fast), 1024);
        return c;
    };
    EXPECT_FALSE(third_block(e).has(TransportInfo::k_jumped));
    EXPECT_FALSE(third_block(0.0).has(TransportInfo::k_jumped));  // change at the block end
    const auto behind = third_block(-10.0 * k_bps);
    EXPECT_TRUE(behind.has(TransportInfo::k_jumped));
    EXPECT_TRUE(behind.has(TransportInfo::k_tempo_changed));
    EXPECT_NEAR(behind.m_jump_delta_beats, -10.0 * k_bps, 1e-12);
    EXPECT_TRUE(third_block(e + (10.0 * k_bps)).has(TransportInfo::k_jumped));
    EXPECT_TRUE(third_block(0.25).has(TransportInfo::k_jumped));  // a seek stays a seek
}

// A source that did not move in the previous block (stopped) gets no tempo term:
// its prediction did not depend on the tempo.
TEST(ContinuityTracker, NoTempoWindowAfterAHeldBlock) {
    ContinuityTracker tracker;
    tracker.prepare(k_sr);
    auto a = raw(2.0, false);
    tracker.resolve(a, 2.0, 1024);
    auto b = raw(2.0, false);
    tracker.resolve(b, 2.0, 1024);
    const double e = 1024 * (180.0 - k_bpm) / (60.0 * k_sr);
    auto c = raw(2.0 + e, true, 180.0);
    tracker.resolve(c, 2.0 + e + (1024 * 180.0 / (60.0 * k_sr)), 1024);
    EXPECT_TRUE(c.has(TransportInfo::k_jumped));
    EXPECT_TRUE(c.has(TransportInfo::k_started));
}
