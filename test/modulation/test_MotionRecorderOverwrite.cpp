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
    EXPECT_NEAR(r.m_x.back(), 0.2f, 1e-6);
    ASSERT_TRUE(r.m_rec.disarm());
    r.step();
    ASSERT_TRUE(r.m_rec.service());
    const MotionLane lane = r.m_rec.lane();
    EXPECT_DOUBLE_EQ(lane.m_length, 8.0);
    EXPECT_DOUBLE_EQ(lane.m_anchor, 0.0);
    ASSERT_EQ(lane.num_points(), 800u);
    EXPECT_EQ(lane.m_gate[100], 1);
    EXPECT_EQ(lane.m_x[100], 0.8f);
    EXPECT_EQ(lane.m_gate[300], 0);  // never touched: lifted, holding the first touch
    EXPECT_EQ(lane.m_x[300], 0.2f);
    EXPECT_EQ(lane.m_gate[500], 1);
    EXPECT_EQ(lane.m_x[500], 0.2f);
}

TEST(MotionRecorderOverwrite, LiftedPartsKeepThePlayingLane) {
    Rig r(256, 120.0);
    start(r);
    r.m_rec.load_lane(motion_test::beats_lane(8.0, 100));
    const MotionLane loaded = r.m_rec.lane();
    r.step();
    ASSERT_TRUE(r.m_rec.arm(LoopLength::Bars2));
    r.run_until(2 * k_beat);
    r.m_pad.touch(1, 0.9f, 0.9f);
    r.run_until(3 * k_beat);
    r.m_pad.release(1);
    r.run_until(6 * k_beat);
    // Lifted: the lane plays on while the take records.
    EXPECT_EQ(r.m_rec.snapshot().m_state, MotionState::Recording);
    const size_t at = r.m_x.size() - 1;
    const double phase = std::fmod(r.m_beat[at], 8.0) / 8.0;
    EXPECT_NEAR(r.m_x[at], 0.5 + (0.4 * std::sin(2.0 * 3.14159265358979323846 * phase)), 1e-3);
    // Next pass: the overdub plays.
    r.run_until(8 * k_beat + 2 * k_beat + k_beat / 2);
    EXPECT_NEAR(r.m_x.back(), 0.9f, 1e-6);

    ASSERT_TRUE(r.m_rec.disarm());
    r.step();
    ASSERT_TRUE(r.m_rec.service());
    const MotionLane lane = r.m_rec.lane();
    ASSERT_EQ(lane.num_points(), 800u);
    for (size_t i = 0; i < lane.num_points(); ++i) {
        if (i >= 200 && i < 300) {
            ASSERT_EQ(lane.m_x[i], 0.9f) << i;
        } else if (i < 190 || i > 310) {
            ASSERT_NEAR(lane.m_x[i], loaded.m_x[i], 1e-4) << i;
            ASSERT_NEAR(lane.m_y[i], loaded.m_y[i], 1e-4) << i;
        }
        ASSERT_EQ(lane.m_gate[i], 1) << i;
    }

    // One take, one undo step.
    ASSERT_TRUE(r.m_rec.undo());
    EXPECT_EQ(r.m_rec.lane().m_x, loaded.m_x);
}

TEST(MotionRecorderOverwrite, UntouchedTakeKeepsTheLane) {
    Rig r(256, 120.0);
    start(r);
    const uint32_t kept = r.m_rec.load_lane(motion_test::beats_lane(4.0, 100));
    r.step();
    ASSERT_TRUE(r.m_rec.record(LoopLength::Bars1));
    r.run_until(3 * k_bar);
    EXPECT_EQ(r.m_rec.snapshot().m_state, MotionState::Recording);
    ASSERT_TRUE(r.m_rec.disarm());
    r.step();
    EXPECT_FALSE(r.m_rec.service());
    EXPECT_EQ(r.m_rec.lane().m_take_id, kept);
    EXPECT_EQ(r.m_rec.snapshot().m_take_id, kept);
    EXPECT_EQ(r.m_rec.snapshot().m_state, MotionState::Playing);
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
