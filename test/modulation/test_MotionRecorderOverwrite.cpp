#include <gtest/gtest.h>
#include <tanh/modulation/MotionLane.h>
#include <tanh/modulation/MotionRecorder.h>
#include <tanh/modulation/MotionTransform.h>

#include <algorithm>
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

TEST(MotionRecorderOverwrite, TakeKeepsRecordingPastItsLoopUntilDisarm) {
    Rig r(256, 120.0);
    start(r);
    ASSERT_TRUE(r.m_rec.arm(LoopLength::Bars1));
    r.m_pad.touch(1, 0.3f, 0.7f);
    r.run_until(k_bar + k_bar / 2);
    EXPECT_EQ(r.m_rec.snapshot().m_state, MotionState::Recording);
    r.m_pad.touch(1, 0.6f, 0.7f);  // the second pass writes over the first
    r.run_until(2 * k_bar + k_beat);
    r.m_pad.release(1);
    r.run_until(3 * k_bar);
    EXPECT_EQ(r.m_rec.snapshot().m_state, MotionState::Recording);
    EXPECT_TRUE(r.m_rec.service());  // the release handed the loop over

    ASSERT_TRUE(r.m_rec.disarm());
    r.step();
    EXPECT_EQ(r.m_rec.snapshot().m_state, MotionState::Playing);
    ASSERT_TRUE(r.m_rec.service());
    const MotionLane lane = r.m_rec.lane();
    EXPECT_DOUBLE_EQ(lane.m_length, 4.0);
    EXPECT_DOUBLE_EQ(lane.m_anchor, 0.0);
    ASSERT_EQ(lane.num_points(), 400u);
    EXPECT_EQ(lane.m_x[50], 0.6f);   // second pass, then the third up to its release
    EXPECT_EQ(lane.m_x[150], 0.3f);  // second pass, before the move
    EXPECT_EQ(lane.m_x[350], 0.6f);  // second pass
    for (size_t i = 0; i < lane.num_points(); ++i) { ASSERT_EQ(lane.m_gate[i], 1) << i; }
}

TEST(MotionRecorderOverwrite, MidLoopStartRecordsOverTheLoopLine) {
    Rig r(256, 120.0);
    start(r);
    ASSERT_TRUE(r.m_rec.arm(LoopLength::Bars2));
    r.run_until(k_bar);  // the take starts halfway into its two bars
    r.m_pad.touch(1, 0.2f, 0.2f);
    r.run_until(2 * k_bar);
    r.m_pad.touch(1, 0.8f, 0.2f);  // past the loop line: the start of the loop
    r.run_until(2 * k_bar + k_bar / 2);
    r.m_pad.release(1);
    r.run_until(3 * k_bar);
    // Lifted after the first loop line: what it recorded plays.
    EXPECT_NEAR(r.m_x.back(), 0.2f, 1e-2);
    ASSERT_TRUE(r.m_rec.disarm());
    r.step();
    ASSERT_TRUE(r.m_rec.service());
    const MotionLane lane = r.m_rec.lane();
    EXPECT_DOUBLE_EQ(lane.m_length, 8.0);
    EXPECT_DOUBLE_EQ(lane.m_anchor, 0.0);
    ASSERT_EQ(lane.num_points(), 800u);
    EXPECT_EQ(lane.m_gate[100], 1);
    EXPECT_EQ(lane.m_x[100], 0.8f);
    EXPECT_EQ(lane.m_gate[300], 1);  // never touched: a glide from 0.8 (199) to 0.2 (400)
    EXPECT_NEAR(lane.m_x[300], 0.5f, 3e-3f);  // halfway, within a point
    EXPECT_EQ(lane.m_gate[500], 1);
    EXPECT_EQ(lane.m_x[500], 0.2f);
}

// A new take starts from nothing: the lane that played stops when it starts, and what the finger
// did not touch is lifted, not the old loop.
TEST(MotionRecorderOverwrite, NewTakeStartsEmpty) {
    Rig r(256, 120.0);
    start(r);
    const uint32_t old = r.m_rec.load_lane(motion_test::beats_lane(8.0, 100));
    r.step();
    ASSERT_TRUE(r.m_rec.arm(LoopLength::Bars2));
    r.run_until(2 * k_beat);
    EXPECT_EQ(r.m_rec.snapshot().m_take_id, old);  // armed: the old lane still plays
    r.m_pad.touch(1, 0.9f, 0.9f);
    r.step();
    EXPECT_EQ(r.m_rec.snapshot().m_take_id, 0u);  // recording: it stopped
    r.run_until(3 * k_beat);
    r.m_pad.release(1);
    r.run_until(6 * k_beat);
    for (size_t i = r.m_gate.size() - 12000; i < r.m_gate.size(); ++i) {
        ASSERT_NEAR(r.m_x[i], 0.9f, 1e-5f) << i;  // only what was drawn, nothing of the old lane
    }
    ASSERT_TRUE(r.m_rec.service());
    const MotionLane lane = r.m_rec.lane();
    ASSERT_EQ(lane.num_points(), 800u);
    EXPECT_EQ(lane.m_x[250], 0.9f);
    EXPECT_EQ(lane.m_gate[250], 1);
    EXPECT_EQ(lane.m_gate[500], 1);
    EXPECT_EQ(lane.m_x[500], 0.9f);  // a glide from the release to the start: one point

    // One take, one undo step: the old lane comes back.
    ASSERT_TRUE(r.m_rec.undo());
    EXPECT_EQ(r.m_rec.lane().m_x, motion_test::beats_lane(8.0, 100).m_x);
}

// A take that keeps nothing still deleted the lane it replaced; undo brings it back.
TEST(MotionRecorderOverwrite, UntouchedTakeDeletesTheLane) {
    Rig r(256, 120.0);
    start(r);
    r.m_rec.load_lane(motion_test::beats_lane(4.0, 100));
    r.step();
    ASSERT_TRUE(r.m_rec.record(LoopLength::Bars1));
    r.run_until(3 * k_bar);
    EXPECT_EQ(r.m_rec.snapshot().m_state, MotionState::Recording);
    EXPECT_EQ(r.m_rec.snapshot().m_take_id, 0u);
    ASSERT_TRUE(r.m_rec.disarm());
    r.step();
    EXPECT_TRUE(r.m_rec.service());
    EXPECT_TRUE(r.m_rec.lane().empty());
    ASSERT_TRUE(r.m_rec.undo());
    EXPECT_FALSE(r.m_rec.lane().empty());
}

namespace {

// Moves every point right by 0.1 (a lane transform).
class Shift final : public thl::modulation::MotionTransform {
public:
    [[nodiscard]] thl::modulation::MotionTransformInfo info(uint64_t settings) const override {
        return {.m_active = settings != 0, .m_uses_lane = true, .m_curve = 1};
    }
    void place(uint64_t /*settings*/, double /*u*/, float& x, float& /*y*/) const override {
        x = std::min(1.0f, x + 0.1f);
    }
    bool unplace(uint64_t settings, float& x, float& /*y*/) const override {
        if (settings != 2) { return false; }  // 2: records through the shift
        x = std::max(0.0f, x - 0.1f);
        return true;
    }
};

}  // namespace

// While a looper keeps recording, what it handed over plays between touches as any lane:
// smoothed and transformed.
TEST(MotionRecorderOverwrite, LiftedLooperPlaysSmoothedAndTransformed) {
    Rig r(256, 120.0);
    start(r);
    const Shift shift;
    r.m_rec.set_transform(&shift);
    r.m_rec.set_transform_settings(1);
    ASSERT_TRUE(r.m_rec.arm(LoopLength::Bars1));
    for (int i = 0; i < 300; ++i) {  // a zigzag over three beats
        r.m_pad.touch(1, (i / 10) % 2 != 0 ? 0.2f : 0.8f, 0.5f);
        r.run_until(r.now() + 240);
    }
    r.m_pad.release(1);
    r.step();
    ASSERT_TRUE(r.m_rec.service());
    r.m_rec.set_smoothing(1.0f);
    r.run_until(r.now() + (2 * k_bar));
    EXPECT_EQ(r.m_rec.snapshot().m_state, MotionState::Recording);
    const MotionLane played = r.m_rec.played_lane();
    const MotionLane raw = r.m_rec.lane();
    double smoothed_err = 0.0;
    double raw_err = 0.0;
    for (size_t i = r.m_x.size() - k_bar; i < r.m_x.size(); i += 97) {
        const double phase = std::fmod(r.m_beat[i], 4.0);
        smoothed_err =
            std::max(smoothed_err,
                     std::abs(static_cast<double>(r.m_x[i]) - (played.sample(phase).m_x + 0.1)));
        raw_err = std::max(raw_err,
                           std::abs(static_cast<double>(r.m_x[i]) - (raw.sample(phase).m_x + 0.1)));
    }
    EXPECT_LT(smoothed_err, 2e-3);
    EXPECT_GT(raw_err, 0.1);

    // Off: the lane as it is.
    r.m_rec.set_transform_settings(0);
    EXPECT_EQ(r.m_rec.transform_version(), 2u);
    r.run_until(r.now() + k_bar);
    const size_t last = r.m_x.size() - 1;
    EXPECT_NEAR(r.m_x[last], played.sample(std::fmod(r.m_beat[last], 4.0)).m_x, 2e-3);
}

// Clearing a looper that still records ends it with nothing kept: recording again starts from an
// empty loop, and the old one never comes back.
TEST(MotionRecorderOverwrite, ClearEndsARunningLooper) {
    Rig r(256, 120.0);
    start(r);
    ASSERT_TRUE(r.m_rec.arm(LoopLength::Bars2));
    r.m_pad.touch(1, 0.2f, 0.2f);
    r.run_until(r.now() + (2 * k_bar));
    r.m_pad.release(1);
    r.step();
    ASSERT_TRUE(r.m_rec.service());
    EXPECT_EQ(r.m_rec.snapshot().m_state, MotionState::Recording);

    r.m_rec.clear();
    r.step();
    EXPECT_EQ(r.m_rec.snapshot().m_state, MotionState::Idle);
    EXPECT_TRUE(r.m_rec.lane().empty());
    ASSERT_TRUE(r.m_rec.disarm());  // record pressed again: nothing to end
    r.step();
    EXPECT_FALSE(r.m_rec.service());
    EXPECT_TRUE(r.m_rec.lane().empty());

    // A new take records from nothing and plays.
    ASSERT_TRUE(r.m_rec.arm(LoopLength::Bars2));
    r.m_pad.touch(1, 0.8f, 0.8f);
    r.run_until(r.now() + k_beat);
    r.m_pad.release(1);
    r.step();
    ASSERT_TRUE(r.m_rec.service());
    const MotionLane lane = r.m_rec.lane();
    for (size_t i = 0; i < lane.num_points(); ++i) { ASSERT_NE(lane.m_x[i], 0.2f) << i; }
    r.run_until(r.now() + k_bar);
    EXPECT_EQ(r.m_rec.snapshot().m_state, MotionState::Recording);
    EXPECT_GT(r.m_rec.snapshot().m_take_id, 0u);
}

// A take handed over just before the clear, not yet published, is dropped with it.
TEST(MotionRecorderOverwrite, ClearDropsAnUnpublishedRevision) {
    Rig r(256, 120.0);
    start(r);
    ASSERT_TRUE(r.m_rec.arm(LoopLength::Bars1));
    r.m_pad.touch(1, 0.2f, 0.2f);
    r.run_until(r.now() + k_beat);
    r.m_pad.release(1);
    r.step();  // handed over, not published
    r.m_rec.clear();
    r.step();
    r.m_rec.service();
    EXPECT_TRUE(r.m_rec.lane().empty());
    r.run_until(r.now() + k_beat);
    EXPECT_EQ(r.m_rec.snapshot().m_take_id, 0u);
    EXPECT_EQ(r.m_gate.back(), 0);
}

// A take records through the transform: what it records plays back where the finger drew.
TEST(MotionRecorderOverwrite, TakeRecordsThroughTheTransform) {
    Rig r(256, 120.0);
    start(r);
    const Shift shift;
    r.m_rec.set_transform(&shift);
    r.m_rec.set_transform_settings(2);
    ASSERT_TRUE(r.m_rec.arm(LoopLength::Bars1));
    r.m_pad.touch(1, 0.6f, 0.5f);
    r.run_until(r.now() + k_bar);
    EXPECT_EQ(r.m_x.back(), 0.6f);  // the finger, live
    r.m_pad.release(1);
    r.step();
    ASSERT_TRUE(r.m_rec.service());
    EXPECT_NEAR(r.m_rec.lane().m_x[200], 0.5f, 1e-6);  // recorded under the shift
    r.run_until(r.now() + k_bar);
    EXPECT_NEAR(r.m_x.back(), 0.6f, 1e-5);  // plays where the finger was
}
