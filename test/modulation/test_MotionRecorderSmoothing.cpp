#include <gtest/gtest.h>
#include <tanh/modulation/MotionLane.h>
#include <tanh/modulation/MotionRecorder.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>

#include "MotionRecorderTestHelpers.h"

using namespace thl::modulation;
using motion_test::Rig;

namespace {

// 120 bpm: one beat = 24000 samples; the test lanes have 100 points per beat.
constexpr uint64_t k_beat = 24000;

Rig& start(Rig& r) {
    r.play_at(0);
    r.step();
    return r;
}

// A 4-beat lane, x = 0.2 with a pulse of 0.8 on [200, 300): the seam is continuous.
MotionLane pulse_lane() {
    MotionLane lane = motion_test::beats_lane(4.0, 100);
    for (size_t i = 0; i < lane.num_points(); ++i) {
        lane.m_x[i] = i >= 200 && i < 300 ? 0.8f : 0.2f;
    }
    return lane;
}

// A 4-beat lane whose x rises from 0.2 to 0.8 with a 0.2 step in the middle:
// a 0.6 jump back at the seam.
MotionLane saw_lane() {
    MotionLane lane = motion_test::beats_lane(4.0, 100);
    for (size_t i = 0; i < lane.num_points(); ++i) {
        const double u = static_cast<double>(i) / 399.0;
        lane.m_x[i] = static_cast<float>(0.2 + (0.4 * u) + (i >= 200 ? 0.2 : 0.0));
    }
    return lane;
}

// Standard deviation in points of a smoothed impulse at index 200.
double impulse_sigma(float amount) {
    MotionRecorder rec;
    rec.prepare(motion_test::k_sr, 256);
    MotionLane lane = motion_test::beats_lane(4.0, 100);
    std::fill(lane.m_x.begin(), lane.m_x.end(), 0.2f);
    lane.m_x[200] = 1.0f;
    rec.set_smoothing(amount);
    rec.load_lane(lane);
    const MotionLane played = rec.played_lane();
    double mass = 0.0;
    double moment = 0.0;
    for (size_t i = 0; i < played.num_points(); ++i) {
        const double w = played.m_x[i] - 0.2;
        const double d = static_cast<double>(i) - 200.0;
        mass += w;
        moment += w * d * d;
    }
    return std::sqrt(moment / mass);
}

// Largest minus smallest x within 480 samples of the loop end at beat 8.
float wrap_span(const Rig& r) {
    const auto mid = static_cast<std::ptrdiff_t>(8 * k_beat);
    const auto [lo, hi] = std::minmax_element(r.m_x.begin() + mid - 480, r.m_x.begin() + mid + 480);
    return *hi - *lo;
}

}  // namespace

TEST(MotionRecorderSmoothing, SmoothingIsZeroPhaseAndNonDestructive) {
    MotionRecorder rec;
    rec.prepare(motion_test::k_sr, 256);
    const MotionLane raw = pulse_lane();
    rec.load_lane(raw);
    EXPECT_EQ(rec.played_lane().m_x, raw.m_x);  // amount 0, continuous seam: raw

    const uint32_t version = rec.lane_version();
    rec.set_smoothing(0.5f);
    EXPECT_FLOAT_EQ(rec.smoothing(), 0.5f);
    EXPECT_GT(rec.lane_version(), version);
    EXPECT_EQ(rec.lane().m_x, raw.m_x);  // the published lane stays raw
    const MotionLane played = rec.played_lane();
    EXPECT_EQ(played.m_gate, raw.m_gate);
    const auto& x = played.m_x;
    EXPECT_GT(x[199], 0.25f);  // both edges are smoothed
    EXPECT_GT(x[300], 0.25f);
    // Zero phase: symmetric around the rising (199 | 200) and falling (299 | 300) edge.
    for (size_t k = 0; k < 50; ++k) {
        EXPECT_NEAR(x[199 - k] + x[200 + k], 1.0f, 1e-5f) << k;
        EXPECT_NEAR(x[299 - k] + x[300 + k], 1.0f, 1e-5f) << k;
    }
}

// sigma = amount^2 * half a beat: 0.02 beat (2 points) at 0.2, half a beat at 1.
// The kernel is cut at 3 sigma, which narrows it by about 1.5 %.
TEST(MotionRecorderSmoothing, SigmaFollowsTheSquaredAmount) {
    EXPECT_NEAR(impulse_sigma(0.2f), 2.0, 0.05);
    EXPECT_NEAR(impulse_sigma(0.5f), 12.5, 0.25);
    EXPECT_NEAR(impulse_sigma(1.0f), 50.0, 1.0);

    // A short Seconds lane: sigma is capped at a sixth of the loop.
    MotionRecorder rec;
    rec.prepare(motion_test::k_sr, 256);
    MotionLane tiny = pulse_lane();
    tiny.m_timebase = MotionTimebase::Seconds;
    tiny.m_length = 0.1;
    rec.set_smoothing(1.0f);
    rec.load_lane(tiny);
    const MotionLane played = rec.played_lane();
    EXPECT_TRUE(motion_test::all_in_unit_range(played.m_x));
    EXPECT_GT(*std::max_element(played.m_x.begin(), played.m_x.end()), 0.3f);
}

// Amount 0 closes the seam over 0.3 beat (30 points) and leaves the rest raw.
TEST(MotionRecorderSmoothing, ZeroAmountClosesTheSeamOverTheBaseWindow) {
    MotionRecorder rec;
    rec.prepare(motion_test::k_sr, 256);
    const MotionLane raw = saw_lane();
    rec.load_lane(raw);
    const MotionLane played = rec.played_lane();
    for (size_t i = 0; i < 370; ++i) { ASSERT_EQ(played.m_x[i], raw.m_x[i]) << i; }
    EXPECT_NEAR(played.m_x[399], raw.m_x[0] - (0.4 / 399.0), 1e-4);  // lands one ramp step short
    EXPECT_EQ(rec.lane().m_x, raw.m_x);
}

// Amount 1 spreads the 0.6 seam jump over the whole loop: no bend at the end.
TEST(MotionRecorderSmoothing, FullAmountSpreadsTheSeamOverTheLoop) {
    MotionRecorder rec;
    rec.prepare(motion_test::k_sr, 256);
    const MotionLane raw = saw_lane();
    rec.set_smoothing(1.0f);
    rec.load_lane(raw);
    const auto& x = rec.played_lane().m_x;
    double max_step = 0.0;
    for (size_t i = 1; i <= 400; ++i) {
        max_step = std::max(max_step, std::abs(static_cast<double>(x[i % 400]) - x[i - 1]));
    }
    EXPECT_LT(max_step, 0.01);  // the 0.6 seam and the 0.2 middle step are both gone
    // The correction builds up over the loop instead of bending only the end.
    EXPECT_LT(x[100] - raw.m_x[100], -0.05);
    EXPECT_LT(x[300] - raw.m_x[300], -0.4);
}

// The published lane keeps the recorded seam, also through JSON; playback closes it.
TEST(MotionRecorderSmoothing, PublishedTakeKeepsTheRawSeam) {
    Rig r(256);
    ASSERT_TRUE(r.m_rec.arm(LoopLength::Free));
    const uint64_t begin = r.now();
    const auto total = static_cast<uint64_t>(motion_test::k_sr);
    while (r.now() < begin + total) {
        const double u = static_cast<double>(r.now() - begin) / static_cast<double>(total);
        r.m_pad.touch(1, static_cast<float>(0.3 + (0.1 * u)), 0.5f);
        r.step();
    }
    r.m_pad.release(1);
    r.step();
    ASSERT_TRUE(r.m_rec.service());
    const MotionLane lane = r.m_rec.lane();
    ASSERT_GT(lane.num_points(), 100u);
    EXPECT_GT(lane.m_x.back() - lane.m_x.front(), 0.095f);
    const auto loaded = MotionLane::from_json(lane.to_json());
    ASSERT_TRUE(loaded.has_value());
    EXPECT_GT(loaded->m_x.back() - loaded->m_x.front(), 0.095f);
    const MotionLane played = r.m_rec.played_lane();
    EXPECT_LT(std::abs(played.m_x.back() - played.m_x.front()), 0.002f);
}

// A bar take started mid-loop records its seam index; JSON keeps it.
TEST(MotionRecorderSmoothing, BarTakeKeepsItsSeamIndex) {
    Rig r(256);
    start(r);
    r.run_until(k_beat + (k_beat / 2));
    ASSERT_TRUE(r.m_rec.arm(LoopLength::Bars1));
    r.m_pad.touch(1, 0.4f, 0.6f);
    r.run_until(r.now() + (5 * k_beat));
    ASSERT_TRUE(r.m_rec.service());
    const MotionLane lane = r.m_rec.lane();
    ASSERT_EQ(lane.num_points(), 400u);
    EXPECT_NEAR(static_cast<double>(lane.m_seam), 151.0, 2.0);
    const auto loaded = MotionLane::from_json(lane.to_json());
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->m_seam, lane.m_seam);
}

// Jump keeps the edge at the loop end and smooths only inside; Smooth closes it.
TEST(MotionRecorderSmoothing, LoopEndJumpKeepsTheEdge) {
    Rig jump(256);
    start(jump);
    jump.m_rec.set_smoothing(0.5f);
    jump.m_rec.set_loop_end(LoopEnd::Jump);
    EXPECT_EQ(jump.m_rec.loop_end(), LoopEnd::Jump);
    jump.m_rec.load_lane(saw_lane());
    const MotionLane played = jump.m_rec.played_lane();
    EXPECT_GT(played.m_x[199], saw_lane().m_x[199] + 0.05f);  // the middle step is smoothed
    EXPECT_GT(played.m_x[399] - played.m_x[0], 0.55f);        // the seam is not
    jump.run_until(9 * k_beat);
    EXPECT_GT(wrap_span(jump), 0.55f);  // one point (240 samples) from 0.8 to 0.2

    Rig smooth(256);
    start(smooth);
    smooth.m_rec.set_smoothing(0.5f);
    smooth.m_rec.load_lane(saw_lane());
    smooth.run_until(9 * k_beat);
    EXPECT_LT(wrap_span(smooth), 0.05f);
}

// A loop end change republishes the same take and playback glides to it.
TEST(MotionRecorderSmoothing, LoopEndChangeRepublishesAndGlides) {
    Rig r(256);
    start(r);
    const uint32_t id = r.m_rec.load_lane(saw_lane());
    r.run_until(3 * k_beat + (k_beat * 9 / 10));  // near the end: the curves differ by ~0.5
    const uint32_t version = r.m_rec.lane_version();
    const MotionLane before = r.m_rec.played_lane();
    r.m_rec.set_loop_end(LoopEnd::Jump);
    r.m_rec.set_loop_end(LoopEnd::Jump);  // no change: no publication
    EXPECT_EQ(r.m_rec.lane_version(), version + 1);
    EXPECT_EQ(r.m_rec.lane().m_take_id, id);
    EXPECT_NE(r.m_rec.played_lane().m_x, before.m_x);
    const size_t from = r.m_x.size();
    r.run_until(r.now() + 2400);
    EXPECT_EQ(r.m_rec.snapshot().m_take_id, id);
    EXPECT_LT(motion_test::max_sample_step(r.m_x, from, from + 2000), 0.002);
}
