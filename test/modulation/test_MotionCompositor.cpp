#include <gtest/gtest.h>
#include <tanh/modulation/detail/MotionCompositor.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

using thl::modulation::detail::MotionCompositor;

namespace {

constexpr double k_sr = 48000.0;
constexpr uint32_t k_block = 256;
constexpr uint32_t k_glide = 1200;  // 25 ms

std::unique_ptr<MotionCompositor> make() {
    auto c = std::make_unique<MotionCompositor>();
    c->prepare(k_sr, 25.0, k_block);
    return c;
}

}  // namespace

TEST(MotionCompositor, TouchOverridesPlayback) {
    auto c = make();
    c->begin(false);
    for (uint32_t i = 0; i < k_block; ++i) {
        c->sample(i, true, 0.2f, 0.3f, true, 0.8f, 0.8f, 1, false);
    }
    EXPECT_EQ(c->x()[100], 0.2f);
    EXPECT_EQ(c->y()[100], 0.3f);
    EXPECT_EQ(c->gate()[100], 1);
    EXPECT_EQ(c->live()[100], 1);
}

TEST(MotionCompositor, ReleaseGlidesBackToPlayback) {
    auto c = make();
    c->begin(false);
    for (uint32_t i = 0; i < k_block; ++i) {
        c->sample(i, i < 100, 0.2f, 0.2f, true, 0.8f, 0.8f, 1, false);
    }
    EXPECT_EQ(c->x()[99], 0.2f);
    EXPECT_EQ(c->live()[100], 0);
    EXPECT_NEAR(c->x()[100], 0.2f, 1e-4f);  // the glide starts where the touch was
    EXPECT_GT(c->x()[255], 0.2f);
    EXPECT_LT(c->x()[255], 0.8f);
    std::vector<float> xs(c->x() + 100, c->x() + k_block);
    for (int b = 0; b < 6; ++b) {
        c->begin(false);
        for (uint32_t i = 0; i < k_block; ++i) {
            c->sample(i, false, 0.0f, 0.0f, true, 0.8f, 0.8f, 1, false);
        }
        xs.insert(xs.end(), c->x(), c->x() + k_block);
    }
    for (size_t i = 1; i < xs.size(); ++i) { ASSERT_LT(std::abs(xs[i] - xs[i - 1]), 2e-3f) << i; }
    EXPECT_EQ(xs[k_glide + 10], 0.8f);  // there after the glide
}

TEST(MotionCompositor, GlideRequestGlidesUnlessTouched) {
    auto c = make();
    c->begin(false);
    for (uint32_t i = 0; i < k_block; ++i) {
        c->sample(i, false, 0.0f, 0.0f, true, 0.3f, 0.3f, 1, false);
    }
    c->begin(true);  // a new lane at 0.9
    c->sample(0, false, 0.0f, 0.0f, true, 0.9f, 0.9f, 1, false);
    EXPECT_NEAR(c->x()[0], 0.3f, 1e-4f);
    EXPECT_EQ(c->change_points().front(), 0u);

    c->begin(true);  // while touched: the touch plays, no glide later
    c->sample(0, true, 0.5f, 0.5f, true, 0.1f, 0.1f, 1, false);
    EXPECT_EQ(c->x()[0], 0.5f);
}

TEST(MotionCompositor, WithoutPlaybackTheOutputHoldsWithTheGateClosed) {
    auto c = make();
    c->begin(false);
    for (uint32_t i = 0; i < k_block; ++i) {
        c->sample(i, i < 10, 0.4f, 0.6f, false, 0.0f, 0.0f, 0, false);
    }
    EXPECT_EQ(c->x()[200], 0.4f);
    EXPECT_EQ(c->y()[200], 0.6f);
    EXPECT_EQ(c->gate()[200], 0);
    EXPECT_EQ(c->gate()[5], 1);
}

TEST(MotionCompositor, ChangePointsAreSortedUniqueEdgesAndTicks) {
    auto c = make();
    c->begin(false);
    for (uint32_t i = 0; i < k_block; ++i) {
        const bool live = i >= 50 && i < 120;
        const uint8_t pg = i < 200 ? 1 : 0;
        c->sample(i, live, 0.5f, 0.5f, true, 0.5f, 0.5f, pg, i % 64 == 0);
    }
    const std::vector<uint32_t> cps(c->change_points().begin(), c->change_points().end());
    const std::vector<uint32_t> expected{0, 50, 64, 120, 128, 192, 200};
    EXPECT_EQ(cps, expected);
}
