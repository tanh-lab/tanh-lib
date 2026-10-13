#include <gtest/gtest.h>
#include <tanh/modulation/MotionTransform.h>
#include <tanh/modulation/detail/MotionClock.h>
#include <tanh/modulation/detail/MotionLanePlayer.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

using thl::modulation::LoopLength;
using thl::modulation::MotionTimebase;
using thl::modulation::MotionTransform;
using thl::modulation::MotionTransformInfo;
using thl::modulation::detail::MotionBlockClock;
using thl::modulation::detail::MotionLanePlayer;
using thl::modulation::detail::MotionLaneSource;
using thl::modulation::detail::MotionPlaySettings;

namespace {

constexpr double k_sr = 48000.0;
constexpr double k_slope = 1.0 / 24000.0;  // 120 bpm
constexpr uint32_t k_block = 256;
constexpr double k_tau = 2.0 * 3.14159265358979323846;

// A loop: x = 0.5 + 0.4 sin, y = 0.5 + 0.4 cos over the loop.
struct Loop {
    std::vector<float> m_x;
    std::vector<float> m_y;
    std::vector<uint8_t> m_gate;
    MotionLaneSource m_source;

    Loop(double length,
         size_t n,
         MotionTimebase timebase = MotionTimebase::Beats,
         double anchor = 0.0,
         uint32_t id = 1) {
        for (size_t i = 0; i < n; ++i) {
            const double ph = k_tau * static_cast<double>(i) / static_cast<double>(n);
            m_x.push_back(static_cast<float>(0.5 + (0.4 * std::sin(ph))));
            m_y.push_back(static_cast<float>(0.5 + (0.4 * std::cos(ph))));
            m_gate.push_back(1);
        }
        m_source.m_id = id;
        m_source.m_version = 1;
        m_source.m_view = {.m_x = m_x.data(),
                           .m_y = m_y.data(),
                           .m_gate = m_gate.data(),
                           .m_num_points = n,
                           .m_timebase = timebase,
                           .m_length = length,
                           .m_anchor = anchor};
        m_source.m_seed = m_source.m_view;
    }
    // The loop's x at @p phase (loop units).
    [[nodiscard]] double x_at(double phase) const {
        return 0.5 + (0.4 * std::sin(k_tau * phase / m_source.m_view.m_length));
    }
};

// Drives a player block by block on a 120 bpm clock and keeps every sample.
struct Player {
    std::unique_ptr<MotionLanePlayer> m_player = std::make_unique<MotionLanePlayer>();
    double m_beat = 0.0;
    double m_slope = k_slope;
    std::vector<float> m_x;
    std::vector<float> m_y;
    std::vector<uint8_t> m_gate;
    std::vector<double> m_beats;
    std::vector<size_t> m_ticks;

    Player() { m_player->prepare(k_sr, 32); }

    /// One block; true if it asked for a glide.
    bool block(const MotionLaneSource& source,
               const MotionPlaySettings& settings = {},
               bool jumped = false,
               bool reset = false) {
        MotionBlockClock clock;
        clock.m_bpm = 120.0;
        clock.m_beat = m_beat;
        clock.m_slope = m_slope;
        clock.m_rec_slope = k_slope;
        clock.m_playing = true;
        clock.m_jumped = jumped;
        clock.m_timeline_reset = reset;
        const bool glide = m_player->begin(clock, source, settings);
        for (uint32_t i = 0; i < k_block; ++i) {
            float x = -1.0f;
            float y = -1.0f;
            uint8_t g = 0;
            if (m_player->playing() && m_player->render(i, x, y, g)) {
                m_ticks.push_back(m_x.size());
            }
            m_x.push_back(x);
            m_y.push_back(y);
            m_gate.push_back(g);
            m_beats.push_back(clock.beat_at(i));
        }
        m_player->end(k_block);
        m_beat = clock.beat_at(k_block);
        return glide;
    }
    void run(const MotionLaneSource& source, int blocks, const MotionPlaySettings& settings = {}) {
        for (int b = 0; b < blocks; ++b) { (void)block(source, settings); }
    }
};

// 1: moves x by +0.1 over a lane; 2 and 3: a circle alone (two curves), gate open.
class TestTransform final : public MotionTransform {
public:
    [[nodiscard]] MotionTransformInfo info(uint64_t settings) const override {
        if (settings == 0) { return {}; }
        return {.m_active = true,
                .m_uses_lane = settings == 1,
                .m_plays_alone = settings >= 2,
                .m_opens_gate = settings >= 2,
                .m_curve = static_cast<uint32_t>(settings)};
    }
    void place(uint64_t settings, double u, float& x, float& y) const override {
        if (settings == 1) {
            x += 0.1f;
            return;
        }
        x = static_cast<float>(0.5 + (0.3 * std::sin(k_tau * u)));
        y = static_cast<float>(0.5 + (0.3 * std::cos(k_tau * u)));
    }
};

}  // namespace

TEST(MotionLanePlayer, BeatsLoopFollowsTheBeat) {
    const Loop loop(4.0, 256);
    Player p;
    EXPECT_TRUE(p.block(loop.m_source));  // a new lane: glide to it
    p.run(loop.m_source, 200);
    for (size_t i = 0; i < p.m_x.size(); i += 101) {
        EXPECT_NEAR(p.m_x[i], loop.x_at(std::fmod(p.m_beats[i], 4.0)), 2e-3) << i;
        EXPECT_EQ(p.m_gate[i], 1);
    }
}

TEST(MotionLanePlayer, AnchorShiftsPointZero) {
    const Loop loop(4.0, 256, MotionTimebase::Beats, 1.0);
    Player p;
    p.run(loop.m_source, 100);
    for (size_t i = 0; i < p.m_x.size(); i += 101) {
        EXPECT_NEAR(p.m_x[i], loop.x_at(std::fmod(p.m_beats[i] + 3.0, 4.0)), 2e-3) << i;
    }
}

TEST(MotionLanePlayer, SecondsLoopRunsOnItsOwnClock) {
    const Loop loop(0.5, 100, MotionTimebase::Seconds);
    Player p;
    p.m_beat = 7.3;
    p.m_slope = 0.0;  // a held clock does not hold a Seconds loop
    p.run(loop.m_source, 100);
    for (size_t i = 0; i < p.m_x.size(); i += 101) {
        const double t = static_cast<double>(i) / k_sr;
        EXPECT_NEAR(p.m_x[i], loop.x_at(std::fmod(t, 0.5)), 3e-3) << i;
    }
}

TEST(MotionLanePlayer, PlaybackLengthStretchesTheLoopOntoBars) {
    const Loop loop(8.0, 512, MotionTimebase::Beats, 3.0);
    Player p;
    const MotionPlaySettings one_bar{.m_length = LoopLength::Bars1};
    p.run(loop.m_source, 200, one_bar);
    for (size_t i = 0; i < p.m_x.size(); i += 101) {
        // Twice as fast, from the bar line (the anchor no longer counts).
        EXPECT_NEAR(p.m_x[i], loop.x_at(2.0 * std::fmod(p.m_beats[i], 4.0)), 3e-3) << i;
    }
}

TEST(MotionLanePlayer, ReverseMirrorsTheBeatPhase) {
    const Loop loop(4.0, 256);
    Player p;
    const MotionPlaySettings reverse{.m_reverse = true};
    p.run(loop.m_source, 100, reverse);
    for (size_t i = 0; i < p.m_x.size(); i += 101) {
        const double mirrored = 4.0 - std::fmod(p.m_beats[i], 4.0);
        EXPECT_NEAR(p.m_x[i], loop.x_at(mirrored), 2e-3) << i;
    }
}

TEST(MotionLanePlayer, OnlyAFlaggedJumpThatMovesThePhaseGlides) {
    const Loop loop(4.0, 256);
    Player p;
    p.run(loop.m_source, 10);
    EXPECT_FALSE(p.block(loop.m_source));
    EXPECT_FALSE(p.block(loop.m_source, {}, true));  // flagged, but continuous
    p.m_beat += 1.0;
    EXPECT_FALSE(p.block(loop.m_source));  // unflagged: followed silently
    EXPECT_NEAR(p.m_x.back(), loop.x_at(std::fmod(p.m_beats.back(), 4.0)), 2e-3);
    p.m_beat += 1.0;
    EXPECT_TRUE(p.block(loop.m_source, {}, true));
    EXPECT_EQ(p.m_player->jump_glides(), 1u);
    p.m_beat += 4.0;  // a whole loop: the same phase
    EXPECT_FALSE(p.block(loop.m_source, {}, true));
    p.m_beat += 1.0;
    EXPECT_FALSE(p.block(loop.m_source, {}, false, true));  // a timeline reset re-locks silently
    EXPECT_NEAR(p.m_x.back(), loop.x_at(std::fmod(p.m_beats.back(), 4.0)), 2e-3);
}

TEST(MotionLanePlayer, NewLaneGlidesAndNewCurveGlidesInPlace) {
    const Loop a(4.0, 256, MotionTimebase::Beats, 0.0, 1);
    Loop b(4.0, 256, MotionTimebase::Beats, 0.0, 2);
    Player p;
    p.run(a.m_source, 10);
    EXPECT_TRUE(p.block(b.m_source));  // another id
    EXPECT_FALSE(p.block(b.m_source));
    b.m_source.m_version = 2;  // the same take, published as it was handed over: silent
    EXPECT_FALSE(p.block(b.m_source));
    b.m_source.m_version = 3;  // smoothed: another curve, glide in place
    b.m_source.m_smoothed = true;
    EXPECT_TRUE(p.block(b.m_source));
    EXPECT_NEAR(p.m_x.back(), b.x_at(std::fmod(p.m_beats.back(), 4.0)), 2e-3);
    p.m_player->restart();  // play() again
    EXPECT_TRUE(p.block(b.m_source));
    p.m_player->reset();  // prepare(): re-lock, no glide
    EXPECT_FALSE(p.block(b.m_source));
}

TEST(MotionLanePlayer, PlaybackLengthOrReverseChangeGlides) {
    const Loop loop(4.0, 256);
    Player p;
    p.run(loop.m_source, 10);
    EXPECT_TRUE(p.block(loop.m_source, {.m_length = LoopLength::Bars2}));
    EXPECT_FALSE(p.block(loop.m_source, {.m_length = LoopLength::Bars2}));
    EXPECT_TRUE(p.block(loop.m_source, {.m_length = LoopLength::Bars2, .m_reverse = true}));
}

TEST(MotionLanePlayer, HeldClockHoldsABeatsLoop) {
    const Loop loop(4.0, 256);
    Player p;
    p.run(loop.m_source, 10);
    p.m_slope = 0.0;
    p.run(loop.m_source, 10);
    const size_t from = p.m_x.size() - (9 * k_block);
    for (size_t i = from; i < p.m_x.size(); ++i) { ASSERT_EQ(p.m_x[i], p.m_x[from]) << i; }
}

TEST(MotionLanePlayer, GateFollowsTheLane) {
    Loop loop(4.0, 256);
    for (size_t i = 128; i < 256; ++i) { loop.m_gate[i] = 0; }  // second half lifted
    Player p;
    p.run(loop.m_source, 200);
    for (size_t i = 0; i < p.m_x.size(); i += 97) {
        const double ph = std::fmod(p.m_beats[i], 4.0);
        if (ph > 0.1 && ph < 1.9) { EXPECT_EQ(p.m_gate[i], 1) << i; }
        if (ph > 2.1 && ph < 3.9) { EXPECT_EQ(p.m_gate[i], 0) << i; }
    }
}

TEST(MotionLanePlayer, NothingToPlayIsNotPlaying) {
    Player p;
    (void)p.block(MotionLaneSource{});
    EXPECT_FALSE(p.m_player->playing());
    EXPECT_EQ(p.m_player->length(), 0.0);
    EXPECT_EQ(p.m_player->take_id(), 0u);
}

TEST(MotionLanePlayer, RendersEveryIntervalWithARampBetween) {
    const Loop loop(4.0, 256);
    Player p;
    p.run(loop.m_source, 4);
    ASSERT_GE(p.m_ticks.size(), 4u * k_block / 32u);
    for (size_t k = 1; k < p.m_ticks.size(); ++k) {
        EXPECT_EQ(p.m_ticks[k] - p.m_ticks[k - 1], 32u) << k;
    }
}

TEST(MotionLanePlayer, TransformMovesTheLane) {
    const Loop loop(4.0, 256);
    const TestTransform transform;
    Player p;
    const MotionPlaySettings moved{.m_transform = &transform, .m_transform_settings = 1};
    p.run(loop.m_source, 100, moved);
    for (size_t i = 0; i < p.m_x.size(); i += 101) {
        EXPECT_NEAR(p.m_x[i], loop.x_at(std::fmod(p.m_beats[i], 4.0)) + 0.1, 2e-3) << i;
    }
    // Without a lane, a transform that needs one plays nothing.
    Player empty;
    (void)empty.block(MotionLaneSource{}, moved);
    EXPECT_FALSE(empty.m_player->playing());
}

TEST(MotionLanePlayer, TransformPlaysAloneOverThePlaybackLength) {
    const TestTransform transform;
    Player p;
    const MotionPlaySettings alone{.m_length = LoopLength::Bars2,
                                   .m_transform = &transform,
                                   .m_transform_settings = 2};
    EXPECT_TRUE(p.block(MotionLaneSource{}, alone));
    p.run(MotionLaneSource{}, 200, alone);
    EXPECT_DOUBLE_EQ(p.m_player->length(), 8.0);
    for (size_t i = 0; i < p.m_x.size(); i += 101) {
        const double u = std::fmod(p.m_beats[i], 8.0) / 8.0;
        EXPECT_NEAR(p.m_x[i], 0.5 + (0.3 * std::sin(k_tau * u)), 2e-3) << i;
        EXPECT_EQ(p.m_gate[i], 1) << i;
    }
    // Free: one bar.
    Player free;
    free.run(MotionLaneSource{}, 2, {.m_transform = &transform, .m_transform_settings = 2});
    EXPECT_DOUBLE_EQ(free.m_player->length(), 4.0);
    // Another curve glides.
    EXPECT_TRUE(p.block(
        MotionLaneSource{},
        {.m_length = LoopLength::Bars2, .m_transform = &transform, .m_transform_settings = 3}));
}
