#include <gtest/gtest.h>
#include <tanh/modulation/MotionLane.h>
#include <tanh/modulation/MotionRecorder.h>
#include <tanh/modulation/MotionShape.h>

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "MotionRecorderTestHelpers.h"

using motion_test::beats_lane;
using motion_test::k_sr;
using motion_test::Rig;
using thl::modulation::k_motion_shape_count;
using thl::modulation::MotionShape;
using thl::modulation::MotionShapeMix;
using thl::modulation::MotionShapePoint;

namespace {

constexpr uint32_t k_block = 256;
// 120 BPM: one 4/4 bar is two seconds.
constexpr uint64_t k_bar = static_cast<uint64_t>(2.0 * k_sr);
// Past the 25 ms glide of the first block.
constexpr uint64_t k_settled = 4096;

MotionShapeMix mix_of(MotionShape a, MotionShape b, float morph = 0.0f) {
    MotionShapeMix mix;
    mix.m_a = a;
    mix.m_b = b;
    mix.m_morph = morph;
    return mix;
}

// Where @p mix should be at output sample @p i of @p r, over a loop of @p beats.
MotionShapePoint expected(const Rig& r,
                          const MotionShapeMix& mix,
                          size_t i,
                          double beats,
                          MotionShapePoint take = {.m_x = 0.5f, .m_y = 0.5f}) {
    const double u = thl::modulation::detail::wrap_phase(r.m_beat[i], beats) / beats;
    return thl::modulation::mix_motion_shapes(mix, u, take);
}

}  // namespace

TEST(MotionShape, EveryShapeIsClosedAndInRange) {
    for (size_t s = 1; s < k_motion_shape_count; ++s) {
        const auto shape = static_cast<MotionShape>(s);
        const MotionShapePoint start = thl::modulation::evaluate_motion_shape(shape, 0.0);
        const MotionShapePoint end = thl::modulation::evaluate_motion_shape(shape, 1.0 - 1e-7);
        EXPECT_NEAR(start.m_x, end.m_x, 1e-3) << thl::modulation::motion_shape_name(shape);
        EXPECT_NEAR(start.m_y, end.m_y, 1e-3) << thl::modulation::motion_shape_name(shape);
        for (int i = 0; i < 1000; ++i) {
            const MotionShapePoint p =
                thl::modulation::evaluate_motion_shape(shape, static_cast<double>(i) / 1000.0);
            EXPECT_LE(std::abs(p.m_x), 1.0f) << thl::modulation::motion_shape_name(shape);
            EXPECT_LE(std::abs(p.m_y), 1.0f) << thl::modulation::motion_shape_name(shape);
        }
    }
}

TEST(MotionShape, SizeAndRotationPlaceTheShape) {
    MotionShapeMix mix = mix_of(MotionShape::Circle, MotionShape::Circle);
    // The circle starts at the top: full size reaches the pad's edge.
    MotionShapePoint p = thl::modulation::place_motion_shape(MotionShape::Circle, mix, 0.0);
    EXPECT_NEAR(p.m_x, 0.5f, 1e-6);
    EXPECT_NEAR(p.m_y, 1.0f, 1e-6);
    // A quarter turn clockwise moves the top to the right, half size halfway.
    mix.m_size = 0.5f;
    mix.m_rotation = 0.25f;
    p = thl::modulation::place_motion_shape(MotionShape::Circle, mix, 0.0);
    EXPECT_NEAR(p.m_x, 0.75f, 1e-6);
    EXPECT_NEAR(p.m_y, 0.5f, 1e-6);
}

TEST(MotionShape, WithoutShapesTheMixIsInactive) {
    const MotionShapeMix mix;
    EXPECT_FALSE(mix.active());
    EXPECT_TRUE(mix_of(MotionShape::Take, MotionShape::Heart).active());
}

TEST(MotionShape, RecorderPlaysAShapeWithoutALane) {
    Rig r(k_block);
    r.play_at(0);
    const MotionShapeMix mix = mix_of(MotionShape::Circle, MotionShape::Circle);
    r.m_rec.set_shapes(mix);
    r.run_until(2 * k_bar);
    // One bar per loop while the playback length is Free.
    for (size_t i = k_settled; i < 2 * k_bar; i += 997) {
        const MotionShapePoint e = expected(r, mix, i, 4.0);
        EXPECT_NEAR(r.m_x[i], e.m_x, 2e-3) << i;
        EXPECT_NEAR(r.m_y[i], e.m_y, 2e-3) << i;
        EXPECT_EQ(r.m_gate[i], 1) << i;
    }
    EXPECT_EQ(r.m_rec.snapshot().m_state, thl::modulation::MotionState::Playing);
}

TEST(MotionShape, MorphCrossfadesTwoShapes) {
    Rig r(k_block);
    r.play_at(0);
    const MotionShapeMix mix = mix_of(MotionShape::Circle, MotionShape::Square, 0.5f);
    r.m_rec.set_shapes(mix);
    r.run_until(k_bar);
    for (size_t i = k_settled; i < k_bar; i += 997) {
        const double u = thl::modulation::detail::wrap_phase(r.m_beat[i], 4.0) / 4.0;
        const MotionShapePoint a = thl::modulation::place_motion_shape(MotionShape::Circle, mix, u);
        const MotionShapePoint b = thl::modulation::place_motion_shape(MotionShape::Square, mix, u);
        EXPECT_NEAR(r.m_x[i], 0.5f * (a.m_x + b.m_x), 2e-3) << i;
        EXPECT_NEAR(r.m_y[i], 0.5f * (a.m_y + b.m_y), 2e-3) << i;
    }
}

TEST(MotionShape, TakeSlotFollowsTheLane) {
    Rig r(k_block);
    r.play_at(0);
    ASSERT_NE(r.m_rec.load_lane(beats_lane(8.0, 64)), 0u);
    // Morph 0: the take as it is; the loop is the lane's eight beats.
    r.m_rec.set_shapes(mix_of(MotionShape::Take, MotionShape::Heart, 0.0f));
    r.run_until(4 * k_bar);
    const MotionShapeMix full = mix_of(MotionShape::Take, MotionShape::Heart, 1.0f);
    r.m_rec.set_shapes(full);
    const uint64_t from = r.now();
    r.run_until(from + (4 * k_bar));
    for (size_t i = k_settled; i < from; i += 997) {
        const double ph = thl::modulation::detail::wrap_phase(r.m_beat[i], 8.0);
        const double angle = 2.0 * 3.14159265358979323846 * ph / 8.0;
        EXPECT_NEAR(r.m_x[i], 0.5 + (0.4 * std::sin(angle)), 3e-3) << i;
    }
    for (size_t i = from + k_settled; i < r.now(); i += 997) {
        const MotionShapePoint e = expected(r, full, i, 8.0);
        EXPECT_NEAR(r.m_x[i], e.m_x, 2e-3) << i;
        EXPECT_NEAR(r.m_y[i], e.m_y, 2e-3) << i;
    }
}

TEST(MotionShape, TouchMovesTheShapeCentre) {
    Rig r(k_block);
    r.play_at(0);
    const MotionShapeMix mix = mix_of(MotionShape::Circle, MotionShape::Circle);
    r.m_rec.set_shapes(mix);
    r.run_until(k_settled);
    r.m_pad.touch(1, 0.7f, 0.4f);
    const uint64_t from = r.now();
    r.run_until(from + k_bar);
    for (size_t i = from + k_block; i < r.now(); i += 997) {
        const MotionShapePoint e = expected(r, mix, i, 4.0);
        EXPECT_NEAR(r.m_x[i], std::clamp(e.m_x + 0.2f, 0.0f, 1.0f), 2e-3) << i;
        EXPECT_NEAR(r.m_y[i], std::clamp(e.m_y - 0.1f, 0.0f, 1.0f), 2e-3) << i;
    }
}

TEST(MotionShape, BothSlotsOnTakePlayTheLaneUnchanged) {
    Rig with(k_block);
    Rig without(k_block);
    for (Rig* r : {&with, &without}) {
        r->play_at(0);
        ASSERT_NE(r->m_rec.load_lane(beats_lane(4.0, 64)), 0u);
    }
    MotionShapeMix mix;
    mix.m_morph = 0.7f;
    mix.m_size = 0.3f;
    with.m_rec.set_shapes(mix);
    with.run_until(2 * k_bar);
    without.run_until(2 * k_bar);
    ASSERT_EQ(with.m_x.size(), without.m_x.size());
    for (size_t i = 0; i < with.m_x.size(); ++i) {
        ASSERT_EQ(with.m_x[i], without.m_x[i]) << i;
        ASSERT_EQ(with.m_y[i], without.m_y[i]) << i;
    }
}

TEST(MotionShape, PlayedPathSamplesTheMix) {
    thl::modulation::MotionRecorder rec;
    rec.prepare(k_sr, 512);
    EXPECT_TRUE(rec.played_path().empty());
    const uint32_t before = rec.shapes_version();
    const MotionShapeMix mix = mix_of(MotionShape::Star, MotionShape::Take);
    rec.set_shapes(mix);
    EXPECT_NE(rec.shapes_version(), before);
    const thl::modulation::MotionLane path = rec.played_path(100);
    ASSERT_EQ(path.num_points(), 100u);
    for (size_t i = 0; i < 100; ++i) {
        const MotionShapePoint e =
            thl::modulation::place_motion_shape(MotionShape::Star,
                                                mix,
                                                static_cast<double>(i) / 100.0);
        EXPECT_FLOAT_EQ(path.m_x[i], e.m_x);
        EXPECT_FLOAT_EQ(path.m_y[i], e.m_y);
        EXPECT_EQ(path.m_gate[i], 1);
    }
}

TEST(MotionShape, ShapesPauseWhileATakeRecords) {
    Rig with(k_block);
    Rig without(k_block);
    with.m_rec.set_shapes(mix_of(MotionShape::Star, MotionShape::Heart, 0.3f));
    for (Rig* r : {&with, &without}) {
        r->play_at(0);
        r->run_until(k_bar / 2);
        ASSERT_TRUE(r->m_rec.arm(thl::modulation::LoopLength::Bars1));
    }
    const uint64_t down = with.now();
    // A finger on a slow line for half a bar, lifted for the rest of the take.
    for (Rig* r : {&with, &without}) {
        while (r->now() < down + (k_bar / 2)) {
            const float t = static_cast<float>(r->now() - down) / static_cast<float>(k_bar);
            r->m_pad.touch(1, 0.2f + t, 0.3f);
            r->step();
        }
        r->m_pad.release(1);
        r->run_until(down + (2 * k_bar));
        ASSERT_TRUE(r->m_rec.service());
    }
    // While the take records (one bar from the touch), the output is the touch or holds; after
    // it the shapes resume (no slot plays the take).
    for (size_t i = down; i < down + k_bar - k_block; ++i) {
        ASSERT_EQ(with.m_x[i], without.m_x[i]) << i;
        ASSERT_EQ(with.m_y[i], without.m_y[i]) << i;
    }
    const thl::modulation::MotionLane a = with.m_rec.lane();
    const thl::modulation::MotionLane b = without.m_rec.lane();
    ASSERT_FALSE(a.empty());
    EXPECT_EQ(a.m_x, b.m_x);
    EXPECT_EQ(a.m_y, b.m_y);
    EXPECT_EQ(a.m_gate, b.m_gate);
}
