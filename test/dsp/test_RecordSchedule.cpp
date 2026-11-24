#include <gtest/gtest.h>
#include <tanh/dsp/transport/RecordSchedule.h>
#include <tanh/dsp/transport/TransportInfo.h>

#include <cstdint>
#include <optional>

using namespace thl::dsp::transport;

namespace {

constexpr double k_rate = 48000.0;
constexpr uint32_t k_block = 512;
// 120 bpm at 48 kHz: 24000 samples a beat, 96000 a bar of 4/4.
constexpr uint64_t k_beat = 24000;
constexpr uint64_t k_bar = 4 * k_beat;

// A transport that runs, block by block, and the schedule it feeds.
struct Rig {
    Rig() { m_schedule.prepare(k_rate); }

    [[nodiscard]] TransportInfo info() const {
        TransportInfo t;
        t.m_flags = TransportInfo::k_has_tempo | TransportInfo::k_has_beat_position |
                    TransportInfo::k_has_time_signature;
        if (m_playing) { t.m_flags |= TransportInfo::k_is_playing; }
        t.m_num_samples = k_block;
        t.m_bpm = m_bpm;
        t.m_beat_position = m_beat;
        t.m_beats_per_sample = m_playing ? m_bpm / 60.0 / k_rate : 0.0;
        t.m_sig_num = m_sig_num;
        t.m_sig_denom = m_sig_denom;
        return t;
    }

    // One block. Counts the take's samples and notes where it started and ended, in samples
    // since the rig was made.
    RecordWindow block(uint32_t extra_flags = 0, double jump_delta = 0.0) {
        TransportInfo t = info();
        t.m_flags |= extra_flags;
        t.m_jump_delta_beats = jump_delta;
        const RecordWindow window = m_schedule.advance(t);
        EXPECT_LE(window.m_begin, window.m_end);
        EXPECT_LE(window.m_end, k_block);
        if (window.m_started) {
            EXPECT_FALSE(m_start.has_value()) << "started twice";
            m_start = m_clock + window.m_begin;
        }
        m_recorded += window.m_end - window.m_begin;
        if (window.m_final) {
            EXPECT_FALSE(m_end.has_value()) << "ended twice";
            m_end = m_clock + window.m_end;
        }
        m_clock += k_block;
        if (m_playing) { m_beat = t.beat_end(); }
        return window;
    }

    // Blocks until the take ends; false when it does not within `limit` blocks.
    bool run_to_end(int limit = 4000) {
        for (int i = 0; i < limit && !m_end; ++i) { block(); }
        return m_end.has_value();
    }

    RecordSchedule m_schedule;
    bool m_playing = true;
    double m_bpm = 120.0;
    double m_beat = 0.0;
    int32_t m_sig_num = 4;
    int32_t m_sig_denom = 4;
    uint64_t m_clock = 0;     // samples since the start
    uint64_t m_recorded = 0;  // samples the windows covered
    std::optional<uint64_t> m_start;
    std::optional<uint64_t> m_end;
};

}  // namespace

TEST(RecordSchedule, IdleRecordsNothing) {
    Rig rig;
    for (int i = 0; i < 10; ++i) {
        const RecordWindow window = rig.block();
        EXPECT_TRUE(window.empty());
        EXPECT_FALSE(window.m_started);
        EXPECT_FALSE(window.m_final);
    }
    EXPECT_EQ(rig.m_schedule.status().m_phase, RecordPhase::Idle);
    rig.m_schedule.stop();  // nothing to stop
    rig.block();
    EXPECT_EQ(rig.m_schedule.phase(), RecordPhase::Idle);
}

// Armed in the middle of a bar: the take starts on the next bar line, at its sample.
TEST(RecordSchedule, StartsOnTheNextBarLineAtItsSample) {
    Rig rig;
    rig.m_beat = 1.5;  // bar line at beat 4: 2.5 beats = 60000 samples ahead
    rig.m_schedule.arm();

    double last_until_start = 4.0;
    while (!rig.m_start) {
        const RecordWindow window = rig.block();
        if (!window.m_started) {
            EXPECT_TRUE(window.empty());
            const RecordStatus status = rig.m_schedule.status();
            EXPECT_EQ(status.m_phase, RecordPhase::Armed);
            EXPECT_LT(status.m_beats_until_start, last_until_start);
            last_until_start = status.m_beats_until_start;
        } else {
            EXPECT_EQ(window.m_begin, 60000U % k_block);
            EXPECT_EQ(window.m_end, k_block);
        }
    }
    EXPECT_EQ(*rig.m_start, 60000U);
    EXPECT_DOUBLE_EQ(rig.m_schedule.timing().m_start_beat, 4.0);
    EXPECT_DOUBLE_EQ(rig.m_schedule.timing().m_bpm, 120.0);

    const RecordStatus status = rig.m_schedule.status();
    EXPECT_EQ(status.m_phase, RecordPhase::Recording);
    EXPECT_TRUE(status.m_on_grid);
    EXPECT_DOUBLE_EQ(status.m_beats_until_start, 0.0);
    EXPECT_DOUBLE_EQ(status.m_length_beats, 0.0) << "open until stop()";
}

TEST(RecordSchedule, ArmedOnABarLineStartsThere) {
    Rig rig;
    rig.m_schedule.arm();
    const RecordWindow window = rig.block();
    EXPECT_TRUE(window.m_started);
    EXPECT_EQ(window.m_begin, 0U);
    EXPECT_EQ(window.m_end, k_block);
    EXPECT_DOUBLE_EQ(rig.m_schedule.timing().m_start_beat, 0.0);
}

// Stop in the middle of the third bar: the take runs to the end of it and is three bars.
TEST(RecordSchedule, StopRunsToTheEndOfTheBar) {
    Rig rig;
    rig.m_schedule.arm();
    while (rig.m_clock < (2 * k_bar) + 30000) { rig.block(); }
    rig.m_schedule.stop();
    rig.block();

    RecordStatus status = rig.m_schedule.status();
    EXPECT_EQ(status.m_phase, RecordPhase::Finishing);
    EXPECT_DOUBLE_EQ(status.m_length_beats, 12.0);
    EXPECT_DOUBLE_EQ(status.length_bars(), 3.0);
    EXPECT_GT(status.m_beats_left, 0.0);
    EXPECT_LT(status.m_beats_left, 4.0);
    EXPECT_NEAR(status.elapsed_bars(), static_cast<double>(rig.m_clock) / k_bar, 1e-9);

    ASSERT_TRUE(rig.run_to_end());
    EXPECT_EQ(*rig.m_end, 3 * k_bar);
    EXPECT_EQ(rig.m_recorded, 3 * k_bar);

    const RecordTiming& timing = rig.m_schedule.timing();
    EXPECT_DOUBLE_EQ(timing.m_length_beats, 12.0);
    EXPECT_EQ(timing.m_length_frames, 3 * k_bar);
    EXPECT_EQ(timing.m_total_frames, 3 * k_bar);

    status = rig.m_schedule.status();
    EXPECT_EQ(status.m_phase, RecordPhase::Idle);
    EXPECT_EQ(status.m_takes_finished, 1U);

    // Nothing after the end.
    EXPECT_TRUE(rig.block().empty());
}

// The shortest take is one bar, even when stop comes right after the start.
TEST(RecordSchedule, ShortestTakeIsOneBar) {
    Rig rig;
    rig.m_schedule.arm();
    rig.block();
    rig.m_schedule.stop();
    ASSERT_TRUE(rig.run_to_end());
    EXPECT_EQ(rig.m_recorded, k_bar);
    EXPECT_DOUBLE_EQ(rig.m_schedule.timing().m_length_beats, 4.0);
}

// Stop that arrives exactly on a bar line ends the take there.
TEST(RecordSchedule, StopOnABarLineEndsThere) {
    Rig rig;
    rig.m_schedule.arm();
    // 96000 is not a multiple of 512: use a tempo whose bar is. 187.5 bpm: 15360 samples a
    // beat, 61440 a bar = 120 blocks.
    rig.m_bpm = 187.5;
    for (int i = 0; i < 240; ++i) { rig.block(); }
    rig.m_schedule.stop();
    const RecordWindow window = rig.block();
    EXPECT_TRUE(window.m_final);
    EXPECT_TRUE(window.empty());
    EXPECT_EQ(rig.m_recorded, 2U * 61440U);
    EXPECT_DOUBLE_EQ(rig.m_schedule.timing().m_length_beats, 8.0);
}

TEST(RecordSchedule, FixedLengthEndsByItself) {
    Rig rig;
    rig.m_beat = 3.0;
    rig.m_schedule.arm(2);
    while (!rig.m_start) { rig.block(); }

    RecordStatus status = rig.m_schedule.status();
    EXPECT_EQ(status.m_phase, RecordPhase::Recording);
    EXPECT_DOUBLE_EQ(status.m_length_beats, 8.0);
    EXPECT_GT(status.m_beats_left, 7.0);

    double last_left = status.m_beats_left;
    while (!rig.m_end) {
        rig.block();
        status = rig.m_schedule.status();
        if (!rig.m_end) {
            EXPECT_LT(status.m_beats_left, last_left);
            last_left = status.m_beats_left;
        }
    }
    EXPECT_EQ(*rig.m_end - *rig.m_start, 2 * k_bar);
    EXPECT_EQ(rig.m_recorded, 2 * k_bar);
    EXPECT_DOUBLE_EQ(rig.m_schedule.timing().m_length_beats, 8.0);
    EXPECT_DOUBLE_EQ(rig.m_schedule.timing().m_start_beat, 4.0);
    EXPECT_EQ(rig.m_schedule.status().m_takes_finished, 1U);
}

// Stop during a fixed-length take ends it earlier, still on a bar line.
TEST(RecordSchedule, StopShortensAFixedLength) {
    Rig rig;
    rig.m_schedule.arm(8);
    while (rig.m_clock < k_bar + 5000) { rig.block(); }
    rig.m_schedule.stop();
    ASSERT_TRUE(rig.run_to_end());
    EXPECT_EQ(rig.m_recorded, 2 * k_bar);
}

// The tail is recorded, and the timing keeps the musical length apart from it.
TEST(RecordSchedule, TailFollowsTheLastBar) {
    Rig rig;
    rig.m_schedule.set_tail_seconds(0.5);
    rig.m_schedule.arm(1);

    bool saw_tail = false;
    while (!rig.m_end) {
        rig.block();
        saw_tail = saw_tail || rig.m_schedule.phase() == RecordPhase::Tail;
    }
    EXPECT_TRUE(saw_tail);
    EXPECT_EQ(rig.m_recorded, k_bar + 24000);
    const RecordTiming& timing = rig.m_schedule.timing();
    EXPECT_DOUBLE_EQ(timing.m_length_beats, 4.0);
    EXPECT_EQ(timing.m_length_frames, k_bar);
    EXPECT_EQ(timing.m_total_frames, k_bar + 24000);
}

// A tail shorter than the rest of the block ends in the block of the bar line.
TEST(RecordSchedule, ShortTailEndsInTheSameBlock) {
    Rig rig;
    rig.m_schedule.set_tail_seconds(10.0 / k_rate);
    rig.m_schedule.arm(1);
    ASSERT_TRUE(rig.run_to_end());
    EXPECT_EQ(rig.m_recorded, k_bar + 10);
    EXPECT_EQ(rig.m_schedule.timing().m_length_frames, k_bar);
}

// Bars are beats, not samples: a tempo change in the take still gives whole bars.
TEST(RecordSchedule, TempoChangeStillGivesWholeBars) {
    Rig rig;
    rig.m_schedule.arm(2);
    while (rig.m_clock < k_bar) { rig.block(); }
    rig.m_bpm = 60.0;  // the second bar takes twice as long
    ASSERT_TRUE(rig.run_to_end());

    EXPECT_DOUBLE_EQ(rig.m_schedule.timing().m_length_beats, 8.0);
    // The block that crossed the first bar line ran a little into the second bar at 120 bpm.
    EXPECT_NEAR(static_cast<double>(rig.m_recorded),
                static_cast<double>(k_bar + (2 * k_bar)),
                2.0 * k_block);
    EXPECT_GT(rig.m_recorded, 2 * k_bar + k_bar / 2);
}

TEST(RecordSchedule, BarFollowsTheTimeSignature) {
    Rig rig;
    rig.m_sig_num = 3;
    rig.m_beat = 0.5;
    rig.m_schedule.arm(2);
    ASSERT_TRUE(rig.run_to_end());
    EXPECT_EQ(*rig.m_start, (5 * k_beat) / 2);  // bar line at beat 3
    EXPECT_EQ(rig.m_recorded, 6 * k_beat);
    EXPECT_DOUBLE_EQ(rig.m_schedule.timing().m_length_beats, 6.0);
    EXPECT_EQ(rig.m_schedule.timing().m_sig_num, 3);
}

// A count-in runs on negative beats: the take starts at beat 0.
TEST(RecordSchedule, CountInStartsAtBeatZero) {
    Rig rig;
    rig.m_beat = -2.0;
    rig.m_schedule.arm(1);
    ASSERT_TRUE(rig.run_to_end());
    EXPECT_EQ(*rig.m_start, 2 * k_beat);
    EXPECT_DOUBLE_EQ(rig.m_schedule.timing().m_start_beat, 0.0);
    EXPECT_EQ(rig.m_recorded, k_bar);
}

// Without a running transport there is no grid: start and stop at once, no musical length.
TEST(RecordSchedule, StoppedTransportRecordsOffTheGrid) {
    Rig rig;
    rig.m_playing = false;
    rig.m_schedule.arm(4);  // the length needs a grid: ignored

    const RecordWindow first = rig.block();
    EXPECT_TRUE(first.m_started);
    EXPECT_EQ(first.m_begin, 0U);
    EXPECT_EQ(first.m_end, k_block);
    RecordStatus status = rig.m_schedule.status();
    EXPECT_EQ(status.m_phase, RecordPhase::Recording);
    EXPECT_FALSE(status.m_on_grid);
    EXPECT_DOUBLE_EQ(status.m_length_beats, 0.0);

    for (int i = 0; i < 99; ++i) { rig.block(); }
    EXPECT_NEAR(rig.m_schedule.status().m_elapsed_seconds, 100.0 * k_block / k_rate, 1e-9);

    rig.m_schedule.stop();
    const RecordWindow last = rig.block();
    EXPECT_TRUE(last.m_final);
    EXPECT_TRUE(last.empty());
    EXPECT_EQ(rig.m_recorded, 100U * k_block);
    EXPECT_DOUBLE_EQ(rig.m_schedule.timing().m_length_beats, 0.0);
    EXPECT_EQ(rig.m_schedule.timing().m_total_frames, 100U * k_block);
}

TEST(RecordSchedule, GridCanBeSwitchedOff) {
    Rig rig;
    rig.m_beat = 1.0;
    rig.m_schedule.set_grid_enabled(false);
    rig.m_schedule.arm();
    EXPECT_TRUE(rig.block().m_started);
    rig.m_schedule.stop();
    EXPECT_TRUE(rig.block().m_final);
    EXPECT_EQ(rig.m_recorded, k_block);
}

// The transport stops under a take on the grid: the take ends there.
TEST(RecordSchedule, TransportStopEndsTheTake) {
    Rig rig;
    rig.m_schedule.arm();
    while (rig.m_clock < k_bar + 20480) { rig.block(); }
    const uint64_t recorded = rig.m_recorded;
    rig.m_playing = false;
    const RecordWindow window = rig.block(TransportInfo::k_stopped);
    EXPECT_TRUE(window.m_final);
    EXPECT_TRUE(window.empty());
    EXPECT_EQ(rig.m_recorded, recorded);

    const RecordTiming& timing = rig.m_schedule.timing();
    EXPECT_NEAR(timing.m_length_beats, static_cast<double>(recorded) / k_beat, 1e-9);
    EXPECT_EQ(timing.m_length_frames, recorded);
    EXPECT_EQ(rig.m_schedule.phase(), RecordPhase::Idle);
}

// A host loop wraps the beat back: the take keeps counting its own bars.
TEST(RecordSchedule, LoopWrapDoesNotDisturbTheCount) {
    Rig rig;
    rig.m_schedule.arm(4);
    // 187.5 bpm: a bar is 120 blocks, so the wrap can sit exactly on a bar line.
    rig.m_bpm = 187.5;
    for (int i = 0; i < 240; ++i) { rig.block(); }
    ASSERT_NEAR(rig.m_beat, 8.0, 1e-9);

    rig.m_beat = 0.0;  // the loop jumps two bars back
    rig.block(TransportInfo::k_jumped, -8.0);
    ASSERT_TRUE(rig.run_to_end());
    EXPECT_EQ(rig.m_recorded, 4U * 61440U);
    EXPECT_DOUBLE_EQ(rig.m_schedule.timing().m_length_beats, 16.0);
}

// A timeline that starts over while playing (a shared session is joined, a clock is reset) is
// no stop: a running take keeps its length, an armed one still waits for its bar line.
TEST(RecordSchedule, TimelineResetIsNotAStop) {
    Rig rig;
    rig.m_bpm = 187.5;  // a bar is 120 blocks
    rig.m_schedule.arm(4);
    for (int i = 0; i < 130; ++i) { rig.block(); }

    rig.m_beat = 100.25;  // the session's beat, somewhere else entirely
    const RecordWindow window = rig.block(TransportInfo::k_timeline_reset);
    EXPECT_FALSE(window.m_final);
    EXPECT_EQ(window.m_end - window.m_begin, k_block);
    EXPECT_EQ(rig.m_schedule.phase(), RecordPhase::Recording);

    ASSERT_TRUE(rig.run_to_end());
    EXPECT_EQ(rig.m_recorded, 4U * 61440U);
    EXPECT_NEAR(rig.m_schedule.timing().m_length_beats, 16.0, 1e-9);

    // Armed when the reset comes: on the grid, at the next bar line of the new timeline.
    Rig armed;
    armed.m_beat = 1.0;
    armed.m_schedule.arm(1);
    const RecordWindow first = armed.block(TransportInfo::k_timeline_reset);
    EXPECT_FALSE(first.m_started);
    EXPECT_EQ(armed.m_schedule.phase(), RecordPhase::Armed);
    ASSERT_TRUE(armed.run_to_end());
    EXPECT_EQ(*armed.m_start, 3 * k_beat);
    EXPECT_EQ(armed.m_recorded, k_bar);
}

// cancel() after arm() leaves nothing armed, also when both come before the next block.
TEST(RecordSchedule, CancelTakesAnArmFromBeforeWithIt) {
    Rig rig;
    rig.m_beat = 1.0;
    rig.m_schedule.arm();
    rig.m_schedule.cancel();
    rig.block();
    EXPECT_EQ(rig.m_schedule.phase(), RecordPhase::Idle);
    for (int i = 0; i < 400; ++i) { EXPECT_TRUE(rig.block().empty()); }

    // The other way round, the arm counts: the old take goes, a new one waits.
    rig.m_schedule.arm();
    while (!rig.m_start) { rig.block(); }
    rig.m_schedule.cancel();
    rig.m_schedule.arm(1);
    rig.block();
    EXPECT_EQ(rig.m_schedule.phase(), RecordPhase::Armed);
}

TEST(RecordSchedule, StopWhileArmedAndCancel) {
    Rig rig;
    rig.m_beat = 1.0;
    rig.m_schedule.arm();
    rig.block();
    EXPECT_EQ(rig.m_schedule.phase(), RecordPhase::Armed);
    rig.m_schedule.stop();
    rig.block();
    EXPECT_EQ(rig.m_schedule.phase(), RecordPhase::Idle);
    EXPECT_EQ(rig.m_schedule.status().m_takes_finished, 0U);

    // Cancel drops a running take without an end.
    rig.m_schedule.arm();
    while (!rig.m_start) { rig.block(); }
    rig.block();
    rig.m_schedule.cancel();
    const RecordWindow window = rig.block();
    EXPECT_TRUE(window.empty());
    EXPECT_FALSE(window.m_final);
    EXPECT_EQ(rig.m_schedule.phase(), RecordPhase::Idle);

    // Arming while a take runs is ignored.
    rig.m_start.reset();
    rig.m_schedule.arm(1);
    while (!rig.m_start) { rig.block(); }
    rig.m_schedule.arm(7);
    ASSERT_TRUE(rig.run_to_end());
    EXPECT_DOUBLE_EQ(rig.m_schedule.timing().m_length_beats, 4.0);
}

// One take after the other, each counted.
TEST(RecordSchedule, TakesFollowEachOther) {
    Rig rig;
    for (uint32_t take = 1; take <= 3; ++take) {
        rig.m_start.reset();
        rig.m_end.reset();
        rig.m_recorded = 0;
        rig.m_schedule.arm(1);
        ASSERT_TRUE(rig.run_to_end());
        EXPECT_EQ(rig.m_recorded, k_bar);
        EXPECT_EQ(*rig.m_start % k_bar, 0U);
        EXPECT_EQ(rig.m_schedule.status().m_takes_finished, take);
    }
}
