#include <gtest/gtest.h>
#include <tanh/modulation/MotionLane.h>
#include <tanh/modulation/MotionRecorder.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>

#include "MotionRecorderTestHelpers.h"

using namespace thl::modulation;
using motion_test::Rig;

namespace {

// 120 bpm, 4/4: one bar = 96000 samples (2 s), 100 points per beat.
constexpr uint64_t k_bar = 96000;

MotionRecorderConfig aligned() {
    MotionRecorderConfig cfg;
    cfg.m_bar_aligned_takes = true;
    return cfg;
}

float circle_x(double seconds) {
    return static_cast<float>(0.5 + (0.3 * std::sin(2.0 * std::numbers::pi * seconds / 0.7)));
}

float circle_y(double seconds) {
    return static_cast<float>(0.5 + (0.3 * std::cos(2.0 * std::numbers::pi * seconds / 0.7)));
}

// Finger on a circle from @p from (its start point before that) until @p to.
void move(Rig& r, uint64_t from, uint64_t to) {
    while (r.now() < to) {
        const double t =
            static_cast<double>(r.now() > from ? r.now() - from : 0) / motion_test::k_sr;
        r.m_pad.touch(1, circle_x(t), circle_y(t));
        r.step();
    }
}

// A free take: touch at @p down, hold still, circle for @p moving samples,
// hold still again, release. Returns the published lane.
MotionLane free_take(Rig& r, uint64_t down, uint64_t moving, uint64_t still) {
    r.run_until(down);
    EXPECT_TRUE(r.m_rec.arm(LoopLength::Free));
    move(r, down + still, down + still + moving);
    r.run_until(down + (2 * still) + moving);
    r.m_pad.release(1);
    r.step();
    EXPECT_TRUE(r.m_rec.service());
    return r.m_rec.lane();
}

// Longest run of output samples where x and y did not change, in [from, to).
size_t longest_frozen_run(const Rig& r, size_t from, size_t to) {
    size_t longest = 0;
    size_t run = 0;
    for (size_t i = from + 1; i < to; ++i) {
        run = r.m_x[i] == r.m_x[i - 1] && r.m_y[i] == r.m_y[i - 1] ? run + 1 : 0;
        longest = std::max(longest, run);
    }
    return longest;
}

}  // namespace

TEST(MotionRecorderFreeTakes, LoopsWithoutAFrozenStartOrEnd) {
    Rig r(256, 120.0, aligned());
    r.play_at(0);
    // Touch at 1.6 bars, 0.3 s still at both ends, 3.2 bars of movement.
    const MotionLane lane = free_take(r, k_bar * 8 / 5, k_bar * 16 / 5, 14400);
    EXPECT_EQ(lane.m_timebase, MotionTimebase::Beats);
    EXPECT_DOUBLE_EQ(lane.m_length, 16.0);
    EXPECT_DOUBLE_EQ(lane.m_anchor, 8.0);  // the bar line nearest the first move
    EXPECT_TRUE(
        std::all_of(lane.m_gate.begin(), lane.m_gate.end(), [](uint8_t g) { return g == 1; }));

    const size_t loop = 4 * k_bar;
    r.run_until(r.now() + (2 * loop));
    const size_t end = r.m_x.size();
    // A finger move lands every 256 samples, a lane point every 240: at most a
    // few render ticks repeat, never the 0.3 s still ends.
    EXPECT_LT(longest_frozen_run(r, end - loop, end), loop / 200);
    EXPECT_LT(motion_test::max_tick_step(r.m_x, end - loop, end), 0.05);
}

TEST(MotionRecorderFreeTakes, LengthSnapsToTheNearestBarCount) {
    Rig r(256, 120.0, aligned());
    r.play_at(0);
    EXPECT_DOUBLE_EQ(free_take(r, k_bar / 4, k_bar * 13 / 10, 4800).m_length, 4.0);
    EXPECT_DOUBLE_EQ(free_take(r, 4 * k_bar, k_bar * 16 / 5, 4800).m_length, 16.0);
    EXPECT_DOUBLE_EQ(free_take(r, 9 * k_bar, k_bar * 6 / 10, 4800).m_length, 4.0);
}

TEST(MotionRecorderFreeTakes, EndsAfterSixteenBarsWithTheFingerDown) {
    Rig r(256, 240.0, aligned());  // one bar = 1 s
    r.play_at(0);
    r.step();
    ASSERT_TRUE(r.m_rec.arm(LoopLength::Free));
    const uint64_t down = r.now();
    move(r, down, down + static_cast<uint64_t>(15.9 * motion_test::k_sr));
    EXPECT_EQ(r.m_rec.snapshot().m_state, MotionState::Recording);
    move(r, down, down + static_cast<uint64_t>(16.1 * motion_test::k_sr));
    EXPECT_EQ(r.m_rec.snapshot().m_state, MotionState::Playing);
    r.m_pad.release(1);
    r.step();
    ASSERT_TRUE(r.m_rec.service());
    EXPECT_DOUBLE_EQ(r.m_rec.lane().m_length, 64.0);
}

TEST(MotionRecorderFreeTakes, DisarmAndStopKeepTheTake) {
    Rig r(256, 120.0, aligned());
    r.play_at(0);
    r.step();
    ASSERT_TRUE(r.m_rec.arm(LoopLength::Free));
    move(r, r.now(), r.now() + (k_bar * 3 / 2));
    ASSERT_TRUE(r.m_rec.disarm());
    r.step();
    r.m_pad.release(1);
    r.step();
    EXPECT_EQ(r.m_rec.snapshot().m_state, MotionState::Playing);
    ASSERT_TRUE(r.m_rec.service());
    EXPECT_DOUBLE_EQ(r.m_rec.lane().m_length, 8.0);  // 1.5 bars -> 2

    ASSERT_TRUE(r.m_rec.arm(LoopLength::Free));
    move(r, r.now(), r.now() + (k_bar * 7 / 10));
    ASSERT_TRUE(r.m_rec.stop());
    r.step();
    r.m_pad.release(1);
    ASSERT_TRUE(r.m_rec.service());
    EXPECT_DOUBLE_EQ(r.m_rec.lane().m_length, 4.0);  // 0.7 bars -> 1
    EXPECT_FALSE(r.m_rec.snapshot().m_playing);
}

TEST(MotionRecorderFreeTakes, RecordWaitsForTheFirstTouch) {
    Rig r(256, 120.0, aligned());
    r.play_at(0);
    r.step();
    ASSERT_TRUE(r.m_rec.record(LoopLength::Free));
    r.run_until(k_bar / 4);
    EXPECT_EQ(r.m_rec.snapshot().m_state, MotionState::Recording);
    const uint64_t down = r.now();
    move(r, down, down + k_bar);
    r.m_pad.release(1);
    r.step();
    ASSERT_TRUE(r.m_rec.service());
    const MotionLane lane = r.m_rec.lane();
    EXPECT_DOUBLE_EQ(lane.m_length, 4.0);
    EXPECT_DOUBLE_EQ(lane.m_anchor, 0.0);
    EXPECT_NEAR(lane.m_x[0], circle_x(0.0), 0.01);  // starts at the touch, not lifted
    EXPECT_EQ(lane.m_gate[0], 1);
}
