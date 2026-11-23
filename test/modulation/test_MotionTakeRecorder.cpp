#include <gtest/gtest.h>
#include <tanh/modulation/MotionTypes.h>
#include <tanh/modulation/detail/MotionTakeRecorder.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

using thl::modulation::LoopLength;
using thl::modulation::MotionRecorderConfig;
using thl::modulation::MotionTimebase;
using thl::modulation::detail::MotionLoopView;
using thl::modulation::detail::MotionTakeRecorder;
using thl::modulation::detail::MotionTakeStart;
using Event = MotionTakeRecorder::Event;

namespace {

// 120 bpm at 48 kHz: 24000 samples per beat, 100 points per beat, 400 per 4/4 bar.
constexpr double k_slope = 1.0 / 24000.0;
constexpr uint32_t k_beat = 24000;

struct Rec {
    std::unique_ptr<MotionTakeRecorder> m_rec = std::make_unique<MotionTakeRecorder>();
    double m_beat = 0.0;
    std::vector<Event> m_events;

    explicit Rec(const MotionRecorderConfig& config = {}) { m_rec->prepare(config); }

    MotionTakeRecorder::Start start(LoopLength length,
                                    double beat,
                                    float x = 0.5f,
                                    float y = 0.5f,
                                    MotionLoopView seed = {}) {
        m_beat = beat;
        return m_rec->start({.m_length = length,
                             .m_bpm = 120.0,
                             .m_sig_num = 4,
                             .m_sig_denom = 4,
                             .m_beat = beat,
                             .m_x = x,
                             .m_y = y,
                             .m_seed = seed});
    }

    /// @p samples samples with the finger at (@p x, @p y) or lifted; the last event that
    /// was not None.
    Event run(uint32_t samples, bool live, float x = 0.5f, float y = 0.5f) {
        Event last = Event::None;
        for (uint32_t i = 0; i < samples; ++i) {
            const Event e = m_rec->step(live, x, y, k_slope);
            m_beat += k_slope;
            if (e != Event::None) {
                last = e;
                m_events.push_back(e);
            }
        }
        return last;
    }
};

// A Beats loop of @p beats with @p per_beat points: x a sine, y a cosine.
struct SeedLoop {
    std::vector<float> m_x;
    std::vector<float> m_y;
    std::vector<uint8_t> m_gate;
    MotionLoopView m_view;

    SeedLoop(double beats, size_t per_beat) {
        const auto n = static_cast<size_t>(beats * static_cast<double>(per_beat));
        for (size_t i = 0; i < n; ++i) {
            const double ph =
                2.0 * 3.14159265358979323846 * static_cast<double>(i) / static_cast<double>(n);
            m_x.push_back(static_cast<float>(0.5 + (0.4 * std::sin(ph))));
            m_y.push_back(static_cast<float>(0.5 + (0.4 * std::cos(ph))));
            m_gate.push_back(1);
        }
        m_view = {.m_x = m_x.data(),
                  .m_y = m_y.data(),
                  .m_gate = m_gate.data(),
                  .m_num_points = n,
                  .m_timebase = MotionTimebase::Beats,
                  .m_length = beats};
    }
};

}  // namespace

// --- BarsN: a looper ---------------------------------------------------------------------------

// The loop runs from the loop line; the time the finger did not draw glides from where it lifted
// to where the drawing starts, around the loop line.
TEST(MotionTakeRecorder, BarsTakeGlidesThroughTheTimeNotDrawn) {
    Rec r;
    ASSERT_EQ(r.start(LoopLength::bars(1), 1.0, 0.2f, 0.7f), MotionTakeRecorder::Start::Started);
    (void)r.run(k_beat / 2, true, 0.2f, 0.7f);                    // beats 1 to 1.5 at 0.2
    EXPECT_EQ(r.run(k_beat / 2, true, 0.6f, 0.7f), Event::None);  // 1.5 to 2 at 0.6
    EXPECT_EQ(r.run(1, false), Event::Revision);                  // the release completes the loop
    const MotionLoopView take = r.m_rec->take();
    EXPECT_EQ(take.m_timebase, MotionTimebase::Beats);
    EXPECT_DOUBLE_EQ(take.m_length, 4.0);
    EXPECT_DOUBLE_EQ(take.m_anchor, 0.0);
    ASSERT_EQ(take.m_num_points, 400u);
    for (size_t i = 0; i < 400; ++i) { ASSERT_EQ(take.m_gate[i], 1) << i; }
    EXPECT_EQ(take.m_x[120], 0.2f);  // drawn
    EXPECT_EQ(take.m_x[180], 0.6f);
    // From 0.6 at point 199 around the loop line to 0.2 at point 100: 301 steps.
    EXPECT_NEAR(take.m_x[349], 0.6f - (0.4f * 150.0f / 301.0f), 1e-5f);
    EXPECT_NEAR(take.m_x[50], 0.6f - (0.4f * 251.0f / 301.0f), 1e-5f);
    EXPECT_EQ(take.m_seam, 200u);  // where the newest point meets the oldest
    EXPECT_TRUE(r.m_rec->active());
}

TEST(MotionTakeRecorder, BarsTakeLoopsAndOverwritesOnlyWhereTheFingerIsDown) {
    Rec r;
    ASSERT_EQ(r.start(LoopLength::bars(1), 1.0, 0.3f, 0.3f), MotionTakeRecorder::Start::Started);
    (void)r.run(k_beat, true, 0.3f, 0.3f);      // beat 1 to 2
    (void)r.run(2 * k_beat, false);             // to the loop line
    (void)r.run(k_beat / 2, true, 0.8f, 0.8f);  // beat 4 to 4.5: the start of the loop
    EXPECT_EQ(r.run(1, false), Event::Revision);
    (void)r.run(4 * k_beat, false);  // a whole pass lifted: nothing changes, no event
    EXPECT_EQ(r.m_events.size(), 2u);
    EXPECT_TRUE(r.m_rec->active());
    EXPECT_EQ(r.m_rec->end(), Event::Finished);
    EXPECT_FALSE(r.m_rec->active());
    const MotionLoopView take = r.m_rec->take();
    EXPECT_EQ(take.m_x[25], 0.8f);  // the second pass
    EXPECT_EQ(take.m_gate[25], 1);
    EXPECT_EQ(take.m_x[150], 0.3f);  // the first pass
    EXPECT_EQ(take.m_gate[150], 1);
    // Never drawn: from 0.8 (point 49) to 0.3 (point 100), and from 0.3 (199) to 0.8 (0).
    EXPECT_NEAR(take.m_x[75], 0.8f - (0.5f * 26.0f / 51.0f), 1e-5f);
    EXPECT_EQ(take.m_gate[75], 1);
    EXPECT_NEAR(take.m_x[250], 0.3f + (0.5f * 51.0f / 201.0f), 1e-5f);
    EXPECT_EQ(take.m_gate[250], 1);
}

TEST(MotionTakeRecorder, BarsTakeHeldOverTheLoopLineRecordsIntoTheStart) {
    Rec r;
    ASSERT_EQ(r.start(LoopLength::bars(2), 4.0, 0.2f, 0.2f), MotionTakeRecorder::Start::Started);
    (void)r.run(4 * k_beat, true, 0.2f, 0.2f);  // beat 4 to 8: the second half
    (void)r.run(2 * k_beat, true, 0.6f, 0.6f);  // over the loop line into the first half
    EXPECT_EQ(r.m_events.size(), 0u);
    EXPECT_EQ(r.m_rec->end(), Event::Finished);
    const MotionLoopView take = r.m_rec->take();
    ASSERT_EQ(take.m_num_points, 800u);
    EXPECT_DOUBLE_EQ(take.m_length, 8.0);
    EXPECT_DOUBLE_EQ(take.m_anchor, 0.0);
    EXPECT_EQ(take.m_x[100], 0.6f);
    EXPECT_EQ(take.m_gate[100], 1);
    EXPECT_NEAR(take.m_x[300], 0.6f - (0.4f * 101.0f / 201.0f), 1e-5f);  // never drawn: a glide
    EXPECT_EQ(take.m_gate[300], 1);
    EXPECT_EQ(take.m_x[500], 0.2f);
    EXPECT_EQ(take.m_gate[500], 1);
    EXPECT_EQ(take.m_seam, 200u);
}

TEST(MotionTakeRecorder, BarsTakeStartsAsItsSeed) {
    const SeedLoop seed(8.0, 100);
    Rec r;
    ASSERT_EQ(r.start(LoopLength::bars(2), 2.0, 0.9f, 0.9f, seed.m_view),
              MotionTakeRecorder::Start::Started);
    (void)r.run(k_beat, true, 0.9f, 0.9f);  // beat 2 to 3
    EXPECT_EQ(r.run(1, false), Event::Revision);
    const MotionLoopView take = r.m_rec->take();
    ASSERT_EQ(take.m_num_points, 800u);
    for (size_t i = 0; i < 800; ++i) {
        if (i >= 200 && i < 300) {
            ASSERT_EQ(take.m_x[i], 0.9f) << i;
        } else {
            ASSERT_NEAR(take.m_x[i], seed.m_x[i], 1e-6) << i;
            ASSERT_NEAR(take.m_y[i], seed.m_y[i], 1e-6) << i;
        }
        ASSERT_EQ(take.m_gate[i], 1) << i;
    }
}

TEST(MotionTakeRecorder, ShorterSeedRepeatsOverTheLoop) {
    const SeedLoop seed(4.0, 50);  // one bar, half the resolution
    Rec r;
    ASSERT_EQ(r.start(LoopLength::bars(2), 0.0, 0.5f, 0.5f, seed.m_view),
              MotionTakeRecorder::Start::Started);
    (void)r.run(10, true, 0.5f, 0.5f);
    (void)r.run(1, false);
    const MotionLoopView take = r.m_rec->take();
    ASSERT_EQ(take.m_num_points, 800u);
    for (size_t i = 100; i < 400; i += 38) {  // even points sit on the seed's points
        EXPECT_NEAR(take.m_x[i], take.m_x[i + 400], 1e-6) << i;
        EXPECT_NEAR(take.m_x[i], seed.m_x[i / 2], 1e-6) << i;
    }
}

TEST(MotionTakeRecorder, UntouchedTakeIsCancelled) {
    const SeedLoop seed(4.0, 100);
    for (const auto length : {LoopLength::free(), LoopLength::bars(1)}) {
        Rec r;
        ASSERT_EQ(r.start(length, 0.0, 0.5f, 0.5f, seed.m_view),
                  MotionTakeRecorder::Start::Started);
        EXPECT_EQ(r.run(12 * k_beat, false), Event::None);
        EXPECT_TRUE(r.m_rec->active());
        EXPECT_EQ(r.m_rec->end(), Event::Cancelled);
        EXPECT_FALSE(r.m_rec->active());
    }
}

TEST(MotionTakeRecorder, BarsProgressIsThePositionInTheLoop) {
    Rec r;
    ASSERT_EQ(r.start(LoopLength::bars(2), 2.0), MotionTakeRecorder::Start::Started);
    EXPECT_NEAR(r.m_rec->progress(), 0.25f, 0.01f);
    (void)r.run(4 * k_beat, true);
    EXPECT_NEAR(r.m_rec->progress(), 0.75f, 0.01f);
    (void)r.run(4 * k_beat, true);
    EXPECT_NEAR(r.m_rec->progress(), 0.25f, 0.01f);  // the next pass
}

TEST(MotionTakeRecorder, LoopThatDoesNotFitIsRefused) {
    MotionRecorderConfig config;
    config.m_max_points = 16;
    Rec r(config);
    EXPECT_EQ(r.start(LoopLength::bars(4), 0.0), MotionTakeRecorder::Start::Refused);
    EXPECT_FALSE(r.m_rec->active());
}

TEST(MotionTakeRecorder, AnyLengthOfBarsLoops) {
    // n / division bars of 4/4 at 400 points per bar, from the multiple at or before the start.
    struct Case {
        LoopLength m_length;
        double m_beats;
    };
    for (const auto& c : {Case{LoopLength::bars(1, 4), 1.0},
                          Case{LoopLength::bars(1, 2), 2.0},
                          Case{LoopLength::bars(3), 12.0},
                          Case{LoopLength::bars(3, 2), 6.0},
                          Case{LoopLength::bars(5), 20.0},
                          Case{LoopLength::bars(32), 128.0}}) {
        Rec r;
        ASSERT_EQ(r.start(c.m_length, c.m_beats + 0.5), MotionTakeRecorder::Start::Started);
        (void)r.run(k_beat / 4, true, 0.3f, 0.6f);
        ASSERT_EQ(r.m_rec->end(), Event::Finished);
        const MotionLoopView take = r.m_rec->take();
        EXPECT_DOUBLE_EQ(take.m_length, c.m_beats);
        EXPECT_EQ(take.m_num_points, static_cast<size_t>(std::lround(c.m_beats * 100.0)));
        EXPECT_DOUBLE_EQ(take.m_anchor, 0.0);  // the loop line is a multiple of its length
    }
}

TEST(MotionTakeRecorder, LongerLoopsNeedMorePoints) {
    // 64 bars at 24 points per beat (the least accepted) need 6144 points.
    MotionRecorderConfig config;
    config.m_max_points = 6143;
    EXPECT_EQ(Rec(config).start(LoopLength::bars(64), 0.0), MotionTakeRecorder::Start::Refused);
    config.m_max_points = 6144;
    EXPECT_EQ(Rec(config).start(LoopLength::bars(64), 0.0), MotionTakeRecorder::Start::Started);
}

TEST(MotionTakeRecorder, TooShortLoopIsRefused) {
    Rec r;  // 1/128 bar: 3 points
    EXPECT_EQ(r.start(LoopLength::bars(1, 128), 0.0), MotionTakeRecorder::Start::Refused);
}

TEST(MotionTakeRecorder, LoopLengthsCompareByValue) {
    static_assert(LoopLength::bars(2, 4) == LoopLength::bars(1, 2));
    static_assert(LoopLength::bars(0, 3) == LoopLength::free());
    static_assert(LoopLength::bars(1) != LoopLength::free());
    static_assert(LoopLength::bars(3, 2) != LoopLength::bars(2, 3));
    EXPECT_DOUBLE_EQ(thl::modulation::loop_length_bars(LoopLength::bars(3, 4)), 0.75);
}

// --- Free: one gesture, snapped onto bars
// ---------------------------------------------------------

TEST(MotionTakeRecorder, FreeTakeWaitsForTheTouchAndEndsOnRelease) {
    Rec r;
    ASSERT_EQ(r.start(LoopLength::free(), 0.0), MotionTakeRecorder::Start::Started);
    EXPECT_EQ(r.run(k_beat, false), Event::None);  // waits
    for (int i = 0; i < 300; ++i) {                // three beats of movement from beat 1
        const float x = 0.1f + (0.7f * static_cast<float>(i) / 300.0f);
        (void)r.run(240, true, x, 0.5f);
    }
    EXPECT_EQ(r.run(1, false), Event::Finished);
    EXPECT_FALSE(r.m_rec->active());
    const MotionLoopView take = r.m_rec->take();
    EXPECT_EQ(take.m_timebase, MotionTimebase::Beats);
    EXPECT_DOUBLE_EQ(take.m_length, 4.0);  // three beats: nearest is one bar
    EXPECT_DOUBLE_EQ(take.m_anchor, 0.0);  // the bar line nearest beat 1
    EXPECT_NEAR(static_cast<double>(take.m_num_points), 300.0, 2.0);
    EXPECT_NEAR(take.m_x[0], 0.1f, 0.01f);
    EXPECT_NEAR(take.m_x[take.m_num_points - 1], 0.8f, 0.01f);
    for (size_t i = 0; i < take.m_num_points; ++i) { ASSERT_EQ(take.m_gate[i], 1) << i; }
}

TEST(MotionTakeRecorder, FreeTakeDropsItsStillEnds) {
    Rec r;
    ASSERT_EQ(r.start(LoopLength::free(), 0.0), MotionTakeRecorder::Start::Started);
    (void)r.run(k_beat, true, 0.2f, 0.5f);  // a beat still
    for (int i = 0; i < 400; ++i) {         // four beats moving
        (void)r.run(240, true, 0.2f + (0.6f * static_cast<float>(i) / 400.0f), 0.5f);
    }
    (void)r.run(k_beat, true, 0.8f, 0.5f);  // a beat still
    EXPECT_EQ(r.run(1, false), Event::Finished);
    const MotionLoopView take = r.m_rec->take();
    EXPECT_DOUBLE_EQ(take.m_length, 4.0);
    EXPECT_NEAR(static_cast<double>(take.m_num_points), 400.0, 3.0);  // moving, plus one each end
    EXPECT_DOUBLE_EQ(take.m_anchor, 0.0);  // first kept point near beat 1: bar line 0
}

TEST(MotionTakeRecorder, FreeLengthSnapsToTheNearestBarCount) {
    const std::vector<std::pair<double, double>> cases{{5.0, 4.0},
                                                       {7.0, 8.0},
                                                       {11.0, 8.0},
                                                       {13.0, 16.0},
                                                       {40.0, 32.0},
                                                       {50.0, 64.0}};
    for (const auto& [beats, snapped] : cases) {
        Rec r;
        ASSERT_EQ(r.start(LoopLength::free(), 0.0), MotionTakeRecorder::Start::Started);
        const auto points = static_cast<int>(beats * 100.0);
        for (int i = 0; i < points; ++i) {
            (void)r.run(240, true, static_cast<float>(i % 100) / 100.0f, 0.5f);
        }
        EXPECT_EQ(r.run(1, false), Event::Finished) << beats;
        EXPECT_DOUBLE_EQ(r.m_rec->take().m_length, snapped) << beats;
    }
}

TEST(MotionTakeRecorder, FreeTakeEndsAfterSixteenBarsWithTheFingerDown) {
    Rec r;
    ASSERT_EQ(r.start(LoopLength::free(), 0.0), MotionTakeRecorder::Start::Started);
    Event e = Event::None;
    for (int i = 0; i < 6500 && e == Event::None; ++i) {
        e = r.run(240, true, static_cast<float>(i % 100) / 100.0f, 0.5f);
    }
    EXPECT_EQ(e, Event::Finished);
    EXPECT_DOUBLE_EQ(r.m_rec->take().m_length, 64.0);
    EXPECT_EQ(r.m_rec->take().m_num_points, 6400u);
}

TEST(MotionTakeRecorder, FreeTakeTooShortIsDropped) {
    Rec r;
    ASSERT_EQ(r.start(LoopLength::free(), 0.0), MotionTakeRecorder::Start::Started);
    (void)r.run(500, true, 0.3f, 0.3f);  // two or three points
    EXPECT_EQ(r.run(1, false), Event::Dropped);
    EXPECT_FALSE(r.m_rec->active());
}

TEST(MotionTakeRecorder, EndKeepsAFreeTakeWhereItIs) {
    Rec r;
    ASSERT_EQ(r.start(LoopLength::free(), 0.0), MotionTakeRecorder::Start::Started);
    for (int i = 0; i < 200; ++i) { (void)r.run(240, true, static_cast<float>(i) / 200.0f, 0.5f); }
    EXPECT_EQ(r.m_rec->end(), Event::Finished);
    EXPECT_DOUBLE_EQ(r.m_rec->take().m_length, 4.0);
}

TEST(MotionTakeRecorder, FreeProgressRunsToTheNextBarCount) {
    Rec r;
    ASSERT_EQ(r.start(LoopLength::free(), 0.0), MotionTakeRecorder::Start::Started);
    EXPECT_FLOAT_EQ(r.m_rec->progress(), 0.0f);
    (void)r.run(2 * k_beat, true, 0.1f, 0.1f);
    EXPECT_NEAR(r.m_rec->progress(), 0.5f, 0.01f);  // half of one bar
    (void)r.run(4 * k_beat, true, 0.2f, 0.1f);
    EXPECT_NEAR(r.m_rec->progress(), 0.75f, 0.01f);  // 1.5 of two bars
}
