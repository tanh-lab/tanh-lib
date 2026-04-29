#include <gtest/gtest.h>

#include <tanh/modulation/ResolvedTarget.h>

using namespace thl::modulation;

TEST(ResolvedTarget, BuildChangePointsFromFlags) {
    MonoBuffers mb(100, /*has_additive=*/true, /*has_replace=*/false);

    mb.m_change_point_flags[5] = 1;
    mb.m_change_point_flags[20] = 1;
    mb.m_change_point_flags[50] = 1;

    mb.build_change_points();

    std::vector<uint32_t> expected = {5, 20, 50};
    EXPECT_EQ(mb.m_change_points, expected);
}

TEST(ResolvedTarget, ClearPerBlock) {
    MonoBuffers mb(100, /*has_additive=*/true, /*has_replace=*/false);

    mb.m_additive_buffer[10] = 42.0f;
    mb.m_change_point_flags[10] = 1;
    mb.m_change_points.push_back(10);

    mb.clear_per_block();

    EXPECT_FLOAT_EQ(mb.m_additive_buffer[10], 0.0f);
    EXPECT_EQ(mb.m_change_point_flags[10], 0);
    EXPECT_TRUE(mb.m_change_points.empty());
}
