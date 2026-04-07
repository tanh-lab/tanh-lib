#include <gtest/gtest.h>

#include <tanh/modulation/ResolvedTarget.h>

using namespace thl::modulation;

TEST(ResolvedTarget, BuildChangePointsFromFlags) {
    ResolvedTarget target;
    target.m_has_mono_additive = true;
    target.resize(100);

    target.m_change_point_flags[5] = true;
    target.m_change_point_flags[20] = true;
    target.m_change_point_flags[50] = true;

    target.build_change_points();

    std::vector<uint32_t> expected = {5, 20, 50};
    EXPECT_EQ(target.m_change_points, expected);
}

TEST(ResolvedTarget, ClearPerBlock) {
    ResolvedTarget target;
    target.m_has_mono_additive = true;
    target.resize(100);

    target.m_additive_buffer[10] = 42.0f;
    target.m_change_point_flags[10] = true;
    target.m_change_points.push_back(10);

    target.clear_per_block();

    EXPECT_FLOAT_EQ(target.m_additive_buffer[10], 0.0f);
    EXPECT_FALSE(target.m_change_point_flags[10]);
    EXPECT_TRUE(target.m_change_points.empty());
}
