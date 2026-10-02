// MotionLane: Catmull-Rom sampling over a looping uniform grid and JSON.

#include <gtest/gtest.h>
#include <tanh/modulation/MotionLane.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>
#include <numbers>
#include <vector>

using namespace thl::modulation;

namespace {

MotionLane sine_lane(size_t n, double length) {
    MotionLane lane;
    lane.m_timebase = MotionTimebase::Seconds;
    lane.m_length = length;
    lane.m_rate = static_cast<double>(n) / length;
    for (size_t i = 0; i < n; ++i) {
        const double ph = 2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(n);
        lane.m_x.push_back(static_cast<float>(0.5 + (0.4 * std::sin(ph))));
        lane.m_y.push_back(static_cast<float>(0.5 + (0.4 * std::cos(ph))));
        lane.m_gate.push_back(1);
    }
    return lane;
}

}  // namespace

TEST(MotionLane, CatmullRomHitsKnots) {
    MotionLane lane;
    lane.m_length = 1.0;
    for (int i = 0; i < 37; ++i) {
        lane.m_x.push_back(static_cast<float>(i % 7) / 7.0f);
        lane.m_y.push_back(static_cast<float>((i * 3) % 11) / 11.0f);
        lane.m_gate.push_back(static_cast<uint8_t>(i % 2));
    }
    const size_t n = lane.num_points();
    for (size_t i = 0; i < n; ++i) {
        const double phase = static_cast<double>(i) * lane.m_length / static_cast<double>(n);
        const MotionPoint p = lane.sample(phase);
        EXPECT_NEAR(p.m_x, lane.m_x[i], 1e-6) << i;
        EXPECT_NEAR(p.m_y, lane.m_y[i], 1e-6) << i;
        EXPECT_EQ(p.m_gate, lane.m_gate[i]) << i;
    }
}

TEST(MotionLane, WrapIsC1) {
    const MotionLane lane = sine_lane(200, 1.0);
    const double h = 1e-6;
    // Value and slope just before and just after the seam (index N-1 → 0).
    const double before = lane.m_length - h;
    const double after = h;
    const double v_before = lane.sample(before).m_x;
    const double v_after = lane.sample(after).m_x;
    EXPECT_NEAR(v_before, v_after, 1e-4);
    const double d = 1e-3;
    const double slope_before = (lane.sample(before).m_x - lane.sample(before - d).m_x) / d;
    const double slope_after = (lane.sample(after + d).m_x - lane.sample(after).m_x) / d;
    EXPECT_NEAR(slope_before, slope_after, 1e-4 * 2.0 * std::numbers::pi * 0.4 * 200.0);
    // The interpolated value tracks the sine everywhere, including the last segment.
    for (int k = 0; k < 2000; ++k) {
        const double ph = static_cast<double>(k) / 2000.0;
        const double ref = 0.5 + (0.4 * std::sin(2.0 * std::numbers::pi * ph));
        EXPECT_NEAR(lane.sample(ph).m_x, ref, 1e-4) << ph;
    }
}

TEST(MotionLane, ClampsOvershoot) {
    MotionLane lane;
    lane.m_length = 1.0;
    for (int i = 0; i < 64; ++i) {
        const float v = (i / 8) % 2 == 0 ? 0.0f : 1.0f;
        lane.m_x.push_back(v);
        lane.m_y.push_back(1.0f - v);
        lane.m_gate.push_back(1);
    }
    for (int k = 0; k < 10000; ++k) {
        const MotionPoint p = lane.sample(static_cast<double>(k) / 10000.0);
        ASSERT_GE(p.m_x, 0.0f);
        ASSERT_LE(p.m_x, 1.0f);
        ASSERT_GE(p.m_y, 0.0f);
        ASSERT_LE(p.m_y, 1.0f);
    }
}

TEST(MotionLane, NegativeAndLargePhasesWrap) {
    const MotionLane lane = sine_lane(64, 2.0);
    EXPECT_FLOAT_EQ(lane.sample(-0.5).m_x, lane.sample(1.5).m_x);
    EXPECT_FLOAT_EQ(lane.sample(1000.25).m_y, lane.sample(0.25).m_y);
    const MotionLane empty;
    EXPECT_EQ(empty.sample(0.3).m_gate, 0);
}

TEST(MotionLane, JsonRoundTrip) {
    MotionLane lane = sine_lane(1600, 8.0);
    lane.m_take_id = 7;
    lane.m_timebase = MotionTimebase::Beats;
    lane.m_rate = 200.0;
    lane.m_anchor = 2.25;
    for (size_t i = 412; i < 600; ++i) { lane.m_gate[i] = 0; }

    const nlohmann::json j = lane.to_json();
    EXPECT_EQ(j["version"], 1);
    EXPECT_EQ(j["timebase"], "beats");
    EXPECT_EQ(j["gate"], nlohmann::json::parse("[[0,1],[412,0],[600,1]]"));

    const auto back = MotionLane::from_json(nlohmann::json::parse(j.dump()));
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back->m_take_id, 7u);
    EXPECT_EQ(back->m_timebase, MotionTimebase::Beats);
    EXPECT_EQ(back->m_rate, lane.m_rate);
    EXPECT_EQ(back->m_length, lane.m_length);
    EXPECT_EQ(back->m_anchor, lane.m_anchor);
    ASSERT_EQ(back->num_points(), lane.num_points());
    EXPECT_EQ(back->m_gate, lane.m_gate);
    for (size_t i = 0; i < lane.num_points(); ++i) {
        ASSERT_LE(std::abs(back->m_x[i] - lane.m_x[i]), 1.0f / 65535.0f) << i;
        ASSERT_LE(std::abs(back->m_y[i] - lane.m_y[i]), 1.0f / 65535.0f) << i;
    }

    // Empty lanes survive a round trip.
    const auto empty = MotionLane::from_json(MotionLane{}.to_json());
    ASSERT_TRUE(empty.has_value());
    EXPECT_TRUE(empty->empty());
}

TEST(MotionLane, JsonRejectsMalformedInput) {
    const nlohmann::json good = sine_lane(16, 1.0).to_json();
    ASSERT_TRUE(MotionLane::from_json(good).has_value());

    auto with = [&](const char* key, const nlohmann::json& value) {
        nlohmann::json j = good;
        j[key] = value;
        return MotionLane::from_json(j);
    };
    EXPECT_FALSE(with("version", 99).has_value());
    EXPECT_FALSE(with("rate", 0.0).has_value());
    EXPECT_FALSE(with("length", -1.0).has_value());
    EXPECT_FALSE(with("length", std::numeric_limits<double>::quiet_NaN()).has_value());
    EXPECT_FALSE(with("rate", "fast").has_value());
    EXPECT_FALSE(with("timebase", "ticks").has_value());
    EXPECT_FALSE(with("x", nlohmann::json::array({1, 2, 3})).has_value());  // size mismatch
    EXPECT_FALSE(with("x", nlohmann::json(std::vector<int>(16, 70000))).has_value());
    EXPECT_FALSE(with("y", nlohmann::json(std::vector<double>(16, 0.5))).has_value());
    EXPECT_FALSE(with("gate", nlohmann::json::parse("[[3,1],[2,0]]")).has_value());  // unsorted
    EXPECT_FALSE(with("gate", nlohmann::json::parse("[[0,2]]")).has_value());
    EXPECT_FALSE(with("gate", nlohmann::json::parse("[[16,1]]")).has_value());
    EXPECT_FALSE(MotionLane::from_json(nlohmann::json::array()).has_value());
    EXPECT_FALSE(MotionLane::from_json(nlohmann::json("lane")).has_value());
    nlohmann::json no_rate = good;
    no_rate.erase("rate");
    EXPECT_FALSE(MotionLane::from_json(no_rate).has_value());
}

TEST(MotionLane, JsonResamplesAboveCapacity) {
    const MotionLane lane = sine_lane(1000, 4.0);
    const auto back = MotionLane::from_json(lane.to_json(), 250);
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back->num_points(), 250u);
    EXPECT_DOUBLE_EQ(back->m_rate, lane.m_rate / 4.0);
    for (int k = 0; k < 400; ++k) {
        const double ph = 4.0 * k / 400.0;
        EXPECT_NEAR(back->sample(ph).m_x, lane.sample(ph).m_x, 2e-4) << ph;
    }
}

// A legacy ElasticFX lane (24 ticks per beat, Beats timebase, 2 bars) plays
// without steps: interpolation spreads each tick's change over the tick.
TEST(MotionLane, Legacy24TpbImportPlaysWithoutStep) {
    MotionLane lane;
    lane.m_timebase = MotionTimebase::Beats;
    lane.m_rate = 24.0;
    lane.m_length = 8.0;
    const size_t n = 24 * 8;
    for (size_t i = 0; i < n; ++i) {
        const double ph = 2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(n);
        lane.m_x.push_back(static_cast<float>(0.5 + (0.45 * std::sin(ph))));
        lane.m_y.push_back(static_cast<float>(0.5 + (0.45 * std::sin(2.0 * ph))));
        lane.m_gate.push_back(1);
    }
    const auto back = MotionLane::from_json(lane.to_json());
    ASSERT_TRUE(back.has_value());
    // Render at 48 kHz, 120 BPM: 1 beat = 24000 samples.
    const double beats_per_sample = 120.0 / (60.0 * 48000.0);
    double max_step = 0.0;
    float prev = back->sample(0.0).m_y;
    for (int s = 1; s < 8 * 24000; ++s) {
        const float v = back->sample(s * beats_per_sample).m_y;
        max_step = std::max(max_step, static_cast<double>(std::abs(v - prev)));
        prev = v;
    }
    // A stepped lane would jump by up to 2π·0.45·2/192 ≈ 0.03 at once.
    EXPECT_LT(max_step, 1e-4);
}
