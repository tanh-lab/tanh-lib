#include <gtest/gtest.h>
#include <tanh/modulation/MotionLane.h>
#include <tanh/modulation/MotionLaneProcessing.h>
#include <tanh/modulation/detail/MotionHistory.h>

#include <cmath>
#include <cstddef>
#include <numbers>

using thl::modulation::LoopEnd;
using thl::modulation::make_played_curve;
using thl::modulation::MotionLane;
using thl::modulation::MotionTimebase;

namespace {

// One beat of a ramp from 0.2 to 0.8: a 0.6 step at the seam.
MotionLane ramp_lane(size_t n = 200) {
    MotionLane lane;
    lane.m_timebase = MotionTimebase::Beats;
    lane.m_length = 1.0;
    lane.m_rate = static_cast<double>(n);
    for (size_t i = 0; i < n; ++i) {
        const float v = 0.2f + (0.6f * static_cast<float>(i) / static_cast<float>(n - 1));
        lane.m_x.push_back(v);
        lane.m_y.push_back(1.0f - v);
        lane.m_gate.push_back(1);
    }
    return lane;
}

}  // namespace

TEST(MotionLaneProcessing, JumpWithoutSmoothingKeepsTheLane) {
    const MotionLane lane = ramp_lane();
    const auto curve = make_played_curve(lane, 0.0f, LoopEnd::Jump);
    EXPECT_EQ(curve.m_x, lane.m_x);
    EXPECT_EQ(curve.m_y, lane.m_y);
    EXPECT_TRUE(curve.m_smoothed);  // Jump differs from how a take buffer plays
}

TEST(MotionLaneProcessing, SmoothLoopEndClosesTheSeam) {
    const MotionLane lane = ramp_lane();
    const auto curve = make_played_curve(lane, 0.0f, LoopEnd::Smooth);
    const size_t n = lane.num_points();
    // The step from the last point to the first is about one point's slope, not 0.6.
    EXPECT_LT(std::abs(curve.m_x[0] - curve.m_x[n - 1]), 0.02f);
    // The start of the loop is untouched; only the window before the seam moves.
    EXPECT_EQ(curve.m_x[0], lane.m_x[0]);
    EXPECT_EQ(curve.m_x[n / 2], lane.m_x[n / 2]);
    EXPECT_FALSE(curve.m_smoothed);
}

TEST(MotionLaneProcessing, SmoothingStaysInRangeAndAverages) {
    MotionLane lane = ramp_lane();
    for (size_t i = 0; i < lane.num_points(); ++i) { lane.m_x[i] = (i % 2 == 0) ? 0.0f : 1.0f; }
    // Jump: no seam closing, only the Gaussian.
    const auto curve = make_played_curve(lane, 1.0f, LoopEnd::Jump);
    for (const float v : curve.m_x) {
        ASSERT_GE(v, 0.0f);
        ASSERT_LE(v, 1.0f);
    }
    EXPECT_NEAR(curve.m_x[100], 0.5f, 0.05f);  // the alternation averages out
}

TEST(MotionLaneProcessing, TooShortLanesPlayRaw) {
    const MotionLane lane = ramp_lane(3);
    const auto curve = make_played_curve(lane, 1.0f, LoopEnd::Smooth);
    EXPECT_EQ(curve.m_x, lane.m_x);
}

TEST(MotionHistory, UndoAndRedoSwapOneLevel) {
    thl::modulation::detail::MotionHistory history;
    EXPECT_FALSE(history.can_undo());
    MotionLane a = ramp_lane(8);
    MotionLane b = ramp_lane(16);
    history.record(a, b.empty());  // b replaced a
    ASSERT_TRUE(history.can_undo());
    auto restored = history.undo(b);
    ASSERT_TRUE(restored.has_value());
    EXPECT_EQ(restored->num_points(), 8u);
    EXPECT_FALSE(history.can_undo());
    ASSERT_TRUE(history.can_redo());
    restored = history.redo(*restored);
    ASSERT_TRUE(restored.has_value());
    EXPECT_EQ(restored->num_points(), 16u);
    history.record(MotionLane{}, true);  // empty replacing empty: no change
    EXPECT_TRUE(history.can_undo());
}
