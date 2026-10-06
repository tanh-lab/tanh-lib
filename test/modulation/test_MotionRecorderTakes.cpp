#include <gtest/gtest.h>
#include <tanh/modulation/MotionLane.h>
#include <tanh/modulation/MotionRecorder.h>

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "MotionRecorderTestHelpers.h"

using namespace thl::modulation;
using motion_test::Rig;

namespace {

// 120 bpm, 4/4: one beat = 24000 samples, one bar = 96000, 100 points per beat.
constexpr uint64_t k_beat = 24000;
constexpr uint64_t k_bar = 4 * k_beat;

MotionRecorderConfig aligned() {
    MotionRecorderConfig cfg;
    cfg.m_bar_aligned_takes = true;
    return cfg;
}

// A playing rig at beat 0.
Rig& start(Rig& r) {
    r.play_at(0);
    r.step();
    return r;
}

// A 4-beat lane: x = 0.2 on the first half, 0.8 on the second, gate open.
MotionLane step_lane() {
    MotionLane lane = motion_test::beats_lane(4.0, 100);
    for (size_t i = 0; i < lane.num_points(); ++i) { lane.m_x[i] = i < 200 ? 0.2f : 0.8f; }
    return lane;
}

}  // namespace

TEST(MotionRecorderTakes, AlignedTakeStartsOnTheBarLineWithLeadingSilence) {
    Rig r(256, 120.0, aligned());
    start(r);
    ASSERT_TRUE(r.m_rec.arm(LoopLength::Bars1));
    r.run_until(2 * k_beat);  // touch only in the second half of the bar
    r.m_pad.touch(1, 0.3f, 0.7f);
    r.run_until(k_bar + 512);
    EXPECT_EQ(r.m_rec.snapshot().m_state, MotionState::Playing);
    r.m_pad.release(1);
    r.step();
    ASSERT_TRUE(r.m_rec.service());
    const MotionLane lane = r.m_rec.lane();
    EXPECT_EQ(lane.m_timebase, MotionTimebase::Beats);
    EXPECT_DOUBLE_EQ(lane.m_length, 4.0);
    EXPECT_DOUBLE_EQ(lane.m_anchor, 0.0);
    ASSERT_EQ(lane.num_points(), 400u);
    EXPECT_EQ(lane.m_gate[0], 0);
    EXPECT_EQ(lane.m_gate[195], 0);
    EXPECT_EQ(lane.m_gate[205], 1);
    EXPECT_EQ(lane.m_gate[399], 1);
    EXPECT_EQ(lane.m_x[0], 0.3f);  // the lead holds the first touched value
    EXPECT_EQ(lane.m_x[100], 0.3f);
}

TEST(MotionRecorderTakes, FreeTakeEndsAtTheNextBoundaryWithTheFingerUp) {
    Rig r(256, 120.0, aligned());
    start(r);
    ASSERT_TRUE(r.m_rec.arm(LoopLength::Free));
    r.m_pad.touch(1, 0.4f, 0.4f);
    r.run_until(k_bar + (3 * k_bar / 10));  // lift at 1.3 bars
    r.m_pad.release(1);
    r.run_until(2 * k_bar - 512);
    EXPECT_EQ(r.m_rec.snapshot().m_state, MotionState::Recording);  // lifts are recorded
    r.run_until(2 * k_bar + 512);
    EXPECT_EQ(r.m_rec.snapshot().m_state, MotionState::Playing);
    ASSERT_TRUE(r.m_rec.service());
    const MotionLane lane = r.m_rec.lane();
    EXPECT_DOUBLE_EQ(lane.m_length, 8.0);
    EXPECT_EQ(lane.m_gate[100], 1);
    EXPECT_EQ(lane.m_gate[700], 0);  // padding after the lift

    // Finger down at 2 bars: the take runs on to 4 bars.
    const uint64_t bar_line = 3 * k_bar;
    r.run_until(bar_line);
    ASSERT_TRUE(r.m_rec.arm(LoopLength::Free));
    r.m_pad.touch(1, 0.6f, 0.6f);
    r.run_until(bar_line + (5 * k_bar / 2));
    r.m_pad.release(1);
    r.run_until(bar_line + (4 * k_bar) + 512);
    EXPECT_EQ(r.m_rec.snapshot().m_state, MotionState::Playing);
    ASSERT_TRUE(r.m_rec.service());
    EXPECT_DOUBLE_EQ(r.m_rec.lane().m_length, 16.0);
    EXPECT_DOUBLE_EQ(r.m_rec.lane().m_anchor, 12.0);  // bar 3 (beat 12) mod 16
}

TEST(MotionRecorderTakes, FreeTakeEndsAfterSixteenBars) {
    Rig r(256, 240.0, aligned());  // one bar = 1 s
    start(r);
    ASSERT_TRUE(r.m_rec.arm(LoopLength::Free));
    r.m_pad.touch(1, 0.3f, 0.3f);
    r.run_until(static_cast<uint64_t>(15.9 * motion_test::k_sr));
    EXPECT_EQ(r.m_rec.snapshot().m_state, MotionState::Recording);
    r.run_until(static_cast<uint64_t>(16.1 * motion_test::k_sr));
    EXPECT_EQ(r.m_rec.snapshot().m_state, MotionState::Playing);  // finger still down
    r.m_pad.release(1);
    r.step();
    ASSERT_TRUE(r.m_rec.service());
    EXPECT_DOUBLE_EQ(r.m_rec.lane().m_length, 64.0);
}

TEST(MotionRecorderTakes, DisarmPadsTheTakeInsteadOfDroppingIt) {
    Rig r(256, 120.0, aligned());
    start(r);
    ASSERT_TRUE(r.m_rec.arm(LoopLength::Bars4));
    r.m_pad.touch(1, 0.4f, 0.4f);
    r.run_until(k_bar / 2);
    r.m_pad.release(1);
    r.run_until(k_bar);
    ASSERT_TRUE(r.m_rec.disarm());
    r.step();  // ends at once, padded to 4 bars
    EXPECT_EQ(r.m_rec.snapshot().m_state, MotionState::Playing);
    ASSERT_TRUE(r.m_rec.service());
    MotionLane lane = r.m_rec.lane();
    EXPECT_DOUBLE_EQ(lane.m_length, 16.0);
    EXPECT_EQ(lane.m_gate[100], 1);
    EXPECT_EQ(lane.m_gate[1000], 0);

    // Free with the finger down at 1.5 bars: the next boundary is 2 bars.
    r.run_until(2 * k_bar);
    ASSERT_TRUE(r.m_rec.arm(LoopLength::Free));
    r.m_pad.touch(1, 0.5f, 0.5f);
    r.run_until(3 * k_bar + (k_bar / 2));
    ASSERT_TRUE(r.m_rec.stop());
    r.step();
    r.m_pad.release(1);
    ASSERT_TRUE(r.m_rec.service());
    EXPECT_DOUBLE_EQ(r.m_rec.lane().m_length, 8.0);
    const uint32_t kept = r.m_rec.lane().m_take_id;

    // An untouched take is dropped and the lane plays on.
    ASSERT_TRUE(r.m_rec.play());
    ASSERT_TRUE(r.m_rec.record(LoopLength::Bars2));
    r.run_until(r.now() + k_beat);
    EXPECT_EQ(r.m_rec.snapshot().m_state, MotionState::Recording);
    ASSERT_TRUE(r.m_rec.disarm());
    r.step();
    EXPECT_FALSE(r.m_rec.service());
    EXPECT_EQ(r.m_rec.lane().m_take_id, kept);
    EXPECT_EQ(r.m_rec.snapshot().m_take_id, kept);
}

TEST(MotionRecorderTakes, OverdubReplacesOnlyTheTouchedSpan) {
    Rig r(256);
    start(r);
    const MotionLane original = motion_test::beats_lane(4.0, 100);
    const uint32_t base = r.m_rec.load_lane(original);
    ASSERT_TRUE(r.m_rec.overdub());
    r.run_until(k_beat);
    const uint64_t down = r.now();
    r.m_pad.touch(1, 0.1f, 0.9f);
    r.run_until(2 * k_beat);
    EXPECT_EQ(r.m_rec.snapshot().m_state, MotionState::Recording);
    EXPECT_EQ(r.m_x.back(), 0.1f);  // live finger while punching
    const uint64_t up = r.now();
    r.m_pad.release(1);
    r.step();
    EXPECT_TRUE(r.m_rec.snapshot().m_overdub);
    ASSERT_TRUE(r.m_rec.service());
    const MotionLane lane = r.m_rec.lane();
    EXPECT_GT(lane.m_take_id, base);
    EXPECT_DOUBLE_EQ(lane.m_length, 4.0);
    ASSERT_EQ(lane.num_points(), original.num_points());
    const auto first = static_cast<size_t>(std::ceil(r.m_beat[down] * 100.0));
    const auto last = static_cast<size_t>(std::floor(r.m_beat[up - 1] * 100.0));
    for (size_t i = 0; i < lane.num_points(); ++i) {
        if (i + 1 >= first && i <= last + 1) {
            if (i > first && i < last) {
                ASSERT_EQ(lane.m_x[i], 0.1f) << i;
                ASSERT_EQ(lane.m_y[i], 0.9f) << i;
            }
            continue;
        }
        ASSERT_EQ(lane.m_x[i], original.m_x[i]) << i;
        ASSERT_EQ(lane.m_y[i], original.m_y[i]) << i;
    }
}

TEST(MotionRecorderTakes, OverdubOnAnEmptyRecorderArmsATake) {
    Rig r(256);
    start(r);
    ASSERT_TRUE(r.m_rec.overdub());
    r.step();
    EXPECT_EQ(r.m_rec.snapshot().m_state, MotionState::Armed);
    r.m_pad.touch(1, 0.5f, 0.5f);
    r.step();
    EXPECT_EQ(r.m_rec.snapshot().m_state, MotionState::Recording);
    ASSERT_TRUE(r.m_rec.disarm());
    r.step();
    EXPECT_FALSE(r.m_rec.snapshot().m_overdub);
}

TEST(MotionRecorderTakes, UndoAndRedoATakeAndAPunch) {
    Rig r(256, 120.0, aligned());
    start(r);
    EXPECT_FALSE(r.m_rec.can_undo());
    r.m_rec.load_lane(motion_test::beats_lane(4.0, 100));
    const MotionLane loaded = r.m_rec.lane();

    // A take replaces the loaded lane; undo brings it back under a new id.
    ASSERT_TRUE(r.m_rec.record(LoopLength::Bars1));
    r.m_pad.touch(1, 0.2f, 0.2f);
    r.run_until(r.now() + k_bar + k_beat);
    r.m_pad.release(1);
    r.step();
    ASSERT_TRUE(r.m_rec.undo());  // publishes the finished take first
    const MotionLane undone = r.m_rec.lane();
    EXPECT_EQ(undone.m_x, loaded.m_x);
    EXPECT_GT(undone.m_take_id, loaded.m_take_id + 1);
    EXPECT_FALSE(r.m_rec.can_undo());
    ASSERT_TRUE(r.m_rec.can_redo());
    ASSERT_TRUE(r.m_rec.redo());
    const MotionLane take = r.m_rec.lane();
    EXPECT_EQ(take.m_x[200], 0.2f);
    EXPECT_GT(take.m_take_id, undone.m_take_id);

    // A punch is one undo step.
    ASSERT_TRUE(r.m_rec.overdub());
    r.step();
    r.m_pad.touch(1, 0.9f, 0.9f);
    r.run_until(r.now() + k_beat);
    r.m_pad.release(1);
    r.step();
    ASSERT_TRUE(r.m_rec.service());
    EXPECT_NE(r.m_rec.lane().m_x, take.m_x);
    EXPECT_FALSE(r.m_rec.can_redo());
    ASSERT_TRUE(r.m_rec.undo());
    EXPECT_EQ(r.m_rec.lane().m_x, take.m_x);
    const uint32_t version = r.m_rec.lane_version();
    r.step();
    EXPECT_EQ(r.m_rec.snapshot().m_take_id, r.m_rec.lane().m_take_id);
    EXPECT_EQ(r.m_rec.lane_version(), version);
}

TEST(MotionRecorderTakes, SmoothingIsZeroPhaseCircularAndNonDestructive) {
    MotionRecorder rec;
    rec.prepare(motion_test::k_sr, 256);
    const MotionLane raw = step_lane();
    rec.load_lane(raw);
    EXPECT_EQ(rec.played_lane().m_x, raw.m_x);  // amount 0: raw

    const uint32_t version = rec.lane_version();
    rec.set_smoothing(0.5f);
    EXPECT_FLOAT_EQ(rec.smoothing(), 0.5f);
    EXPECT_GT(rec.lane_version(), version);
    EXPECT_EQ(rec.lane().m_x, raw.m_x);  // the published lane stays raw
    const MotionLane played = rec.played_lane();
    EXPECT_EQ(played.m_gate, raw.m_gate);
    const auto& x = played.m_x;
    EXPECT_GT(x[199], 0.2f);  // the step is smoothed
    EXPECT_LT(x[200], 0.8f);
    // Zero phase: symmetric around the step (199 | 200) and around the seam (399 | 0).
    for (size_t k = 0; k < 50; ++k) {
        EXPECT_NEAR(x[199 - k] + x[200 + k], 1.0f, 1e-5f) << k;
        EXPECT_NEAR(x[399 - k] + x[k], 1.0f, 1e-5f) << k;
    }
    EXPECT_LT(std::abs(x[0] - x[399]), 0.1f);  // the seam is as smooth as the middle step
    EXPECT_NEAR(std::abs(x[0] - x[399]), std::abs(x[200] - x[199]), 1e-5f);
}

TEST(MotionRecorderTakes, SmoothingChangeGlidesWithoutAJump) {
    Rig r(256);
    start(r);
    r.m_rec.load_lane(step_lane());
    r.run_until(k_beat + (k_beat / 2));  // inside the x = 0.2 half
    const size_t from = r.m_x.size();
    r.m_rec.set_smoothing(1.0f);
    r.run_until(r.now() + k_beat);
    // The new curve is 0.014 off at the change: the glide spreads it.
    EXPECT_LT(motion_test::max_sample_step(r.m_x, from, from + 2400), 0.002);
    // A tenth of a beat before the step the smoothed curve already rises (raw: 0.2).
    EXPECT_GT(r.m_x[static_cast<size_t>(1.9 * k_beat)], 0.35f);
    EXPECT_TRUE(motion_test::all_in_unit_range(r.m_x));
}
