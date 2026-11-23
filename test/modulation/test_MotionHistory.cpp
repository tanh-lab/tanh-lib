#include <gtest/gtest.h>
#include <tanh/modulation/MotionLane.h>
#include <tanh/modulation/detail/MotionHistory.h>

#include <cstddef>

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
