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

// A playing rig at beat 0.
Rig& start(Rig& r) {
    r.play_at(0);
    r.step();
    return r;
}

}  // namespace

TEST(MotionRecorderTakes, BarTakeStartsOnTheBarLineWithLeadingSilence) {
    Rig r(256, 120.0);
    start(r);
    ASSERT_TRUE(r.m_rec.arm(LoopLength::Bars1));
    r.run_until(2 * k_beat);  // touch only in the second half of the bar
    r.m_pad.touch(1, 0.3f, 0.7f);
    r.run_until(k_bar - 512);
    r.m_pad.release(1);
    r.step();  // the release hands the loop over; the take loops on until disarm
    EXPECT_EQ(r.m_rec.snapshot().m_state, MotionState::Recording);
    ASSERT_TRUE(r.m_rec.service());
    const MotionLane lane = r.m_rec.lane();
    EXPECT_EQ(lane.m_timebase, MotionTimebase::Beats);
    EXPECT_DOUBLE_EQ(lane.m_length, 4.0);
    EXPECT_DOUBLE_EQ(lane.m_anchor, 0.0);
    ASSERT_EQ(lane.num_points(), 400u);
    EXPECT_EQ(lane.m_gate[0], 0);
    EXPECT_EQ(lane.m_gate[195], 0);
    EXPECT_EQ(lane.m_gate[205], 1);
    EXPECT_EQ(lane.m_gate[390], 1);
    EXPECT_EQ(lane.m_gate[399], 0);  // after the release: lifted
    EXPECT_EQ(lane.m_x[0], 0.3f);    // the lead holds the first touched value
    EXPECT_EQ(lane.m_x[100], 0.3f);
}

TEST(MotionRecorderTakes, DisarmPadsTheTakeInsteadOfDroppingIt) {
    Rig r(256, 120.0);
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
    const MotionLane lane = r.m_rec.lane();
    EXPECT_DOUBLE_EQ(lane.m_length, 16.0);
    EXPECT_EQ(lane.m_gate[100], 1);
    EXPECT_EQ(lane.m_gate[1000], 0);

    const uint32_t kept = lane.m_take_id;

    // An untouched take keeps nothing, and the lane it replaced stays deleted.
    ASSERT_TRUE(r.m_rec.record(LoopLength::Bars2));
    r.run_until(r.now() + k_beat);
    EXPECT_EQ(r.m_rec.snapshot().m_state, MotionState::Recording);
    ASSERT_TRUE(r.m_rec.disarm());
    r.step();
    EXPECT_TRUE(r.m_rec.service());
    EXPECT_GT(r.m_rec.lane().m_take_id, kept);
    EXPECT_TRUE(r.m_rec.lane().empty());
}

TEST(MotionRecorderTakes, UndoAndRedoATakeAndAClear) {
    Rig r(256, 120.0);
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

    // A clear is one undo step; a new publication drops the redo lane.
    r.m_rec.clear();
    EXPECT_TRUE(r.m_rec.lane().empty());
    ASSERT_TRUE(r.m_rec.undo());
    EXPECT_EQ(r.m_rec.lane().m_x, take.m_x);
    EXPECT_TRUE(r.m_rec.can_redo());
    r.m_rec.clear();
    EXPECT_FALSE(r.m_rec.can_redo());
    ASSERT_TRUE(r.m_rec.undo());
    EXPECT_EQ(r.m_rec.lane().m_x, take.m_x);
    const uint32_t version = r.m_rec.lane_version();
    r.step();
    EXPECT_EQ(r.m_rec.snapshot().m_take_id, r.m_rec.lane().m_take_id);
    EXPECT_EQ(r.m_rec.lane_version(), version);
}

// A take started without a touch (record()) replaces the lane at once, however long it loops.
TEST(MotionRecorderTakes, UntouchedTakeReplacesTheLane) {
    Rig r(256, 120.0);
    start(r);
    r.m_rec.load_lane(motion_test::beats_lane(4.0, 100));
    ASSERT_TRUE(r.m_rec.record(LoopLength::Bars1));
    r.run_until(r.now() + (3 * k_bar));
    EXPECT_EQ(r.m_rec.snapshot().m_state, MotionState::Recording);
    EXPECT_EQ(r.m_rec.snapshot().m_take_id, 0u);
    ASSERT_TRUE(r.m_rec.disarm());
    r.step();
    EXPECT_TRUE(r.m_rec.service());
    EXPECT_TRUE(r.m_rec.lane().empty());
    EXPECT_EQ(r.m_rec.snapshot().m_state, MotionState::Idle);
}

// A take that keeps nothing (too short) still deleted the lane it replaced.
TEST(MotionRecorderTakes, DroppedTakeDeletesTheLane) {
    Rig r(256, 120.0);
    start(r);
    r.m_rec.load_lane(motion_test::beats_lane(4.0, 100));
    r.run_until(r.now() + k_beat);
    ASSERT_TRUE(r.m_rec.arm(LoopLength::Free));
    r.m_pad.touch(1, 0.3f, 0.3f);
    r.step();
    EXPECT_EQ(r.m_rec.snapshot().m_state, MotionState::Recording);
    r.m_pad.release(1);
    r.step();  // too short: dropped
    r.run_until(r.now() + k_beat);
    EXPECT_EQ(r.m_gate.back(), 0);
    EXPECT_TRUE(r.m_rec.service());
    EXPECT_TRUE(r.m_rec.lane().empty());
}
