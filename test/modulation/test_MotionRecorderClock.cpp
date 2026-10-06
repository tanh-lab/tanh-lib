#include <gtest/gtest.h>
#include <tanh/dsp/transport/TransportInfo.h>
#include <tanh/modulation/MotionLane.h>
#include <tanh/modulation/MotionRecorder.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <utility>
#include <vector>

#include "MotionRecorderTestHelpers.h"

using namespace thl::modulation;
using motion_test::k_sr;
using motion_test::Rig;
using thl::dsp::transport::TransportInfo;

namespace {

constexpr uint64_t k_spb = 24000;     // samples per beat at 120 BPM
constexpr double k_lane_beats = 4.0;  // 1 bar
constexpr uint32_t k_tpb = 100;
constexpr size_t k_glide = 1200;  // 25 ms
constexpr double k_tick_beats = 1.0 / k_tpb;

// Largest change of the 1-bar test lane per 32 samples at 120 BPM.
constexpr double k_in_loop = 0.4 * 2.0 * std::numbers::pi / k_lane_beats * 32.0 / k_spb;
// A 25 ms raised-cosine glide over the lane's full range (0.1 .. 0.9).
constexpr double k_glide_step = 0.8 * std::numbers::pi / 2.0 * 32.0 / k_glide;

double circ(double a, double b, double len) {
    const double d = std::abs(a - b);
    return std::min(d, len - d);
}

std::unique_ptr<Rig> playing_rig(uint32_t block, double bpm = 120.0) {
    auto r = std::make_unique<Rig>(block, bpm);
    r->play_at(0);
    r->m_rec.load_lane(motion_test::beats_lane(k_lane_beats, k_tpb));
    return r;
}

// Output in [0, 1], finite, and no 32-sample step above the bound.
void check_output(const Rig& r, size_t from, bool glides_allowed, double tempo_scale = 1.0) {
    EXPECT_TRUE(motion_test::all_in_unit_range(r.m_x));
    EXPECT_TRUE(motion_test::all_in_unit_range(r.m_y));
    const double in_loop = k_in_loop * tempo_scale;
    const double bound = glides_allowed ? in_loop + k_glide_step : 1.5 * in_loop;
    EXPECT_LE(motion_test::max_tick_step(r.m_x, from, r.m_x.size()), bound);
    EXPECT_LE(motion_test::max_tick_step(r.m_y, from, r.m_y.size()), bound);
}

// Playback phase read back from the output (the lane is a sine/cosine circle).
double phase_at(const Rig& r, size_t s) {
    return motion_test::sine_phase_of(r.m_x[s], r.m_y[s], k_lane_beats);
}

// Max circular distance between the output phase and the transport phase in [from, to).
double phase_error(const Rig& r, size_t from, size_t to, double offset = 0.0) {
    double m = 0.0;
    for (size_t s = from; s < to && s < r.m_x.size(); s += 17) {
        const double want =
            std::fmod(std::fmod(r.m_beat[s] + offset, k_lane_beats) + k_lane_beats, k_lane_beats);
        m = std::max(m, circ(phase_at(r, s), want, k_lane_beats));
    }
    return m;
}

}  // namespace

class MotionRecorderClock : public ::testing::TestWithParam<uint32_t> {};

INSTANTIATE_TEST_SUITE_P(Blocks,
                         MotionRecorderClock,
                         ::testing::Values(64u, 256u, 1024u),
                         [](const auto& info) { return "Block" + std::to_string(info.param); });

TEST_P(MotionRecorderClock, SeekWhilePlaying) {
    const uint32_t block = GetParam();
    auto rp = playing_rig(block);
    Rig& r = *rp;
    r.run_until(4 * k_spb);
    EXPECT_LE(phase_error(r, 2 * k_glide, r.m_x.size()), k_tick_beats);
    const uint64_t at = r.now() + 1000;  // applies at the next block boundary
    r.seek_at(at, 13.3);
    r.run_until(at + (2 * k_spb));
    EXPECT_EQ(r.m_rec.jump_glide_count(), 1u);
    // The seek lands at the first block start >= at.
    const size_t jump = ((at + block - 1) / block) * block;
    EXPECT_NEAR(r.m_beat[jump], 13.3, 1e-9);
    // Within 25 ms (+ one render tick) the output is on the new phase.
    EXPECT_LE(phase_error(r, jump + k_glide + 32, r.m_x.size()), k_tick_beats);
    // During the glide the distance to the running playback only shrinks.
    const MotionLane lane = motion_test::beats_lane(k_lane_beats, k_tpb);
    double prev = 1e9;
    for (size_t s = jump; s < jump + k_glide; s += 32) {
        const MotionPoint p = lane.sample(std::fmod(r.m_beat[s], k_lane_beats));
        const double d = std::hypot(r.m_x[s] - p.m_x, r.m_y[s] - p.m_y);
        EXPECT_LE(d, prev + 2e-3) << s - jump;
        prev = d;
    }
    check_output(r, 2 * k_glide, true);
}

TEST_P(MotionRecorderClock, TransportJumpGlides) {
    const uint32_t block = GetParam();
    auto rp = playing_rig(block);
    Rig& r = *rp;
    r.run_until(2 * k_spb);
    const uint64_t at = r.now();
    r.seek_at(at, (static_cast<double>(at) / k_spb) + 3.0);  // seek by 3 beats
    r.run_until(at + k_spb);
    EXPECT_EQ(r.m_rec.jump_glide_count(), 1u);
    // The output reaches the new phase within 25 ms ± 1 tick.
    EXPECT_LE(phase_error(r, at + k_glide + 32, r.m_x.size()), k_tick_beats);
    EXPECT_GT(phase_error(r, at, at + 64), 10.0 * k_tick_beats);  // it did not jump
    check_output(r, 2 * k_glide, true);
}

// A 4-bar DAW loop over a 1-bar lane: the wrap does not move the lane, so the
// output is the same as without the loop and nothing glides.
TEST_P(MotionRecorderClock, HostLoopAlignedIsSeamless) {
    const uint32_t block = GetParam();
    auto looped = playing_rig(block);
    auto plain = playing_rig(block);
    looped->loop_at(0, 0.0, 16.0);
    const uint64_t len = 40 * k_spb;  // two and a half DAW loops
    looped->run_until(len);
    plain->run_until(len);
    EXPECT_EQ(looped->m_rec.jump_glide_count(), 0u);
    ASSERT_EQ(looped->m_x.size(), plain->m_x.size());
    double max_diff = 0.0;
    for (size_t i = 0; i < plain->m_x.size(); ++i) {
        max_diff =
            std::max(max_diff, static_cast<double>(std::abs(looped->m_x[i] - plain->m_x[i])));
        max_diff =
            std::max(max_diff, static_cast<double>(std::abs(looped->m_y[i] - plain->m_y[i])));
        ASSERT_EQ(looped->m_gate[i], plain->m_gate[i]);
    }
    // The recorder keeps its running phase across the wrap; what remains is one
    // float ulp (measured 1.2e-7) from the clock's slope, which HostTransportClock
    // derives from beats of different magnitude (16.x vs 0.x).
    EXPECT_LE(max_diff, 1e-6);
    check_output(*looped, 2 * k_glide, false);
}

// A 3-beat DAW loop over a 1-bar lane: one glide per wrap, phase right afterwards.
TEST_P(MotionRecorderClock, HostLoopMisaligned) {
    const uint32_t block = GetParam();
    auto rp = playing_rig(block);
    Rig& r = *rp;
    r.loop_at(0, 0.0, 3.0);
    r.run_until(20 * k_spb);  // 20 beats: 6 wraps
    EXPECT_EQ(r.m_rec.jump_glide_count(), 6u);
    // Away from the glides the phase is the transport's.
    for (size_t s = 2 * k_glide; s < r.m_x.size(); s += 101) {
        const double since_wrap = std::fmod(r.m_beat[s], 3.0) * k_spb;
        if (since_wrap < k_glide + 32 + block) { continue; }
        const double want = std::fmod(r.m_beat[s], k_lane_beats);
        ASSERT_LE(circ(phase_at(r, s), want, k_lane_beats), k_tick_beats) << s;
    }
    check_output(r, 2 * k_glide, true);
}

// Link joining a session moves the beat to the session's phase: +0.37 beats
// glides; a whole multiple of the lane length changes nothing.
TEST_P(MotionRecorderClock, LinkJoinRealign) {
    const uint32_t block = GetParam();
    auto rp = playing_rig(block);
    Rig& r = *rp;
    r.run_until(3 * k_spb);
    const uint64_t at = r.now();
    r.shift_at(at, 0.37);
    r.run_until(at + k_spb);
    EXPECT_EQ(r.m_rec.jump_glide_count(), 1u);
    EXPECT_LE(phase_error(r, at + k_glide + 32, r.m_x.size()), k_tick_beats);

    const uint64_t at2 = r.now();
    r.shift_at(at2, 2.0 * k_lane_beats);
    const size_t before = r.m_x.size();
    r.run_until(at2 + k_spb);
    EXPECT_EQ(r.m_rec.jump_glide_count(), 1u);  // no new glide
    EXPECT_LE(phase_error(r, before, r.m_x.size()), k_tick_beats);
    check_output(r, 2 * k_glide, true);
}

// A peer changes the tempo at an arbitrary sample: no glide, continuous phase,
// and the loop follows the new tempo.
TEST_P(MotionRecorderClock, PeerTempoChangeMidLoop) {
    const uint32_t block = GetParam();
    auto rp = playing_rig(block);
    Rig& r = *rp;
    r.run_until(2 * k_spb);
    r.tempo_at(2 * k_spb + 12345, 97.0);
    r.run_until(20 * k_spb);
    EXPECT_EQ(r.m_rec.jump_glide_count(), 0u);
    EXPECT_LE(phase_error(r, 2 * k_glide, r.m_x.size()), k_tick_beats);
    check_output(r, 2 * k_glide, false);
    // Loop duration at 97 BPM from upward crossings of x through 0.5.
    std::vector<size_t> ups;
    for (size_t s = 4 * k_spb; s < r.m_x.size(); ++s) {
        if (r.m_x[s - 1] < 0.5f && r.m_x[s] >= 0.5f) { ups.push_back(s); }
    }
    ASSERT_GE(ups.size(), 2u);
    for (size_t k = 1; k < ups.size(); ++k) {
        EXPECT_NEAR(static_cast<double>(ups[k] - ups[k - 1]), 4.0 * 60.0 / 97.0 * k_sr, 2.0);
    }
}

// FreeRun: a stop keeps the lane moving at the last tempo; a start that resets
// the beat (Link start/stop sync to the quantum) re-locks with a glide.
// Hold: the output stands still while stopped.
TEST_P(MotionRecorderClock, StartStopSync) {
    const uint32_t block = GetParam();
    {
        auto rp = playing_rig(block);
        Rig& r = *rp;
        r.run_until(3 * k_spb);
        const uint64_t stop = r.now();
        const double stop_beat = static_cast<double>(stop) / k_spb;
        r.stop_at(stop);
        r.run_until(stop + (2 * k_spb));
        // Still moving at 120 BPM from the stop beat.
        for (size_t s = stop + 64; s < r.m_x.size(); s += 97) {
            const double want =
                std::fmod(stop_beat + (static_cast<double>(s - stop) / k_spb), k_lane_beats);
            ASSERT_LE(circ(phase_at(r, s), want, k_lane_beats), k_tick_beats) << s - stop;
        }
        EXPECT_EQ(r.m_rec.jump_glide_count(), 0u);
        const uint64_t start = r.now();
        r.seek_at(start, 0.0);
        r.play_at(start);
        r.run_until(start + (2 * k_spb));
        EXPECT_EQ(r.m_rec.jump_glide_count(), 1u);
        EXPECT_LE(phase_error(r, start + k_glide + 32, r.m_x.size()), k_tick_beats);
        check_output(r, 2 * k_glide, true);
    }
    {
        auto rp = playing_rig(block);
        Rig& r = *rp;
        r.m_rec.set_stopped_transport(StoppedTransport::Hold);
        r.run_until(3 * k_spb);
        const uint64_t stop = r.now();
        r.stop_at(stop);
        r.run_until(stop + (2 * k_spb));
        for (size_t s = stop + 1; s < r.m_x.size(); ++s) {  // held from the first held sample
            ASSERT_EQ(r.m_x[s], r.m_x[stop]) << s - stop;
            ASSERT_EQ(r.m_y[s], r.m_y[stop]) << s - stop;
        }
    }
}

// The flags are set but the beat is continuous (a host that reports a jump onto
// the same position): the lane phase does not move, so nothing glides and the
// output stays seamless.
TEST_P(MotionRecorderClock, HintWithoutJump) {
    const uint32_t block = GetParam();
    auto rp = playing_rig(block);
    Rig& r = *rp;
    r.run_until(2 * k_spb);
    r.m_extra_flags = TransportInfo::k_jumped | TransportInfo::k_timeline_reset;
    r.step();
    r.m_extra_flags = TransportInfo::k_started;
    r.step();
    r.run_until(4 * k_spb);
    EXPECT_EQ(r.m_rec.jump_glide_count(), 0u);
    check_output(r, 2 * k_glide, false);
}

// The clock is the only jump detector. The recorder takes the transport's beat
// every block, and only k_jumped / k_started / k_timeline_reset decide what a
// change of the lane phase means: a jump glides, a timeline reset re-locks at
// once, an unflagged change is taken silently (a clock that reports none has
// nothing to report). A timeline reset while stopped restarts the free-run from
// the transport's beat.
TEST(MotionRecorderClockFlags, OnlyTheFlagsDecideAboutAGlide) {
    constexpr uint32_t k_n = 256;
    const MotionLane lane = motion_test::beats_lane(k_lane_beats, k_tpb);
    auto run = [&](uint32_t flags, bool playing) {
        MotionRecorder rec;
        rec.prepare(k_sr, k_n);
        rec.load_lane(lane);
        TransportInfo t;
        t.m_flags = TransportInfo::k_has_tempo | TransportInfo::k_has_beat_position;
        t.m_bpm = 120.0;
        t.m_num_samples = k_n;
        double beat = 0.0;
        for (int b = 0; b < 200; ++b) {
            t.m_flags &= ~(TransportInfo::k_discontinuity_mask | TransportInfo::k_is_playing);
            const bool now_playing = playing || b < 100;
            if (now_playing) { t.m_flags |= TransportInfo::k_is_playing; }
            t.m_beats_per_sample = now_playing ? 120.0 / (60.0 * k_sr) : 0.0;
            if (b == 150) {
                beat += 1.3;  // the transport moves by 1.3 beats
                t.m_flags |= flags;
            }
            t.m_beat_position = beat;
            rec.process(t, MotionInput{}, k_n);
            beat = t.beat_end();
        }
        return std::pair<uint64_t, double>{rec.jump_glide_count(), rec.playback_phase()};
    };
    const double moved = std::fmod((200 * k_n * 120.0 / (60.0 * k_sr)) + 1.3, k_lane_beats);
    const double unmoved = std::fmod(200 * k_n * 120.0 / (60.0 * k_sr), k_lane_beats);

    const auto unflagged = run(0u, true);
    EXPECT_EQ(unflagged.first, 0u);
    EXPECT_NEAR(unflagged.second, moved, 1e-9);

    const auto jumped = run(TransportInfo::k_jumped, true);
    EXPECT_EQ(jumped.first, 1u);
    EXPECT_NEAR(jumped.second, moved, 1e-9);

    const auto reset = run(TransportInfo::k_jumped | TransportInfo::k_timeline_reset, true);
    EXPECT_EQ(reset.first, 0u);
    EXPECT_NEAR(reset.second, moved, 1e-9);

    // Stopped from block 100 (FreeRun at 120 BPM from the stop beat): a seek is
    // ignored until the next start, a timeline reset re-anchors the free-run.
    const auto stopped_seek = run(TransportInfo::k_jumped, false);
    EXPECT_EQ(stopped_seek.first, 0u);
    EXPECT_NEAR(stopped_seek.second, unmoved, 1e-9);
    const auto stopped_reset = run(TransportInfo::k_timeline_reset, false);
    EXPECT_EQ(stopped_reset.first, 0u);
    EXPECT_NEAR(
        stopped_reset.second,
        std::fmod((100 * k_n * 120.0 / (60.0 * k_sr)) + 1.3 + (50 * k_n * 120.0 / (60.0 * k_sr)),
                  k_lane_beats),
        1e-9);
}

namespace {

// Drive a 2-bar take whose input x encodes the take's own clock: x = (beat mod 8) / 8
// on the recorder's internal beat (start beat + elapsed samples at the current tempo).
struct ClockTake {
    uint64_t m_start = 0;
    uint64_t m_finish = 0;
    double m_start_beat = 0.0;
};

ClockTake record_clock_take(Rig& r,
                            uint64_t until,
                            const std::vector<std::pair<uint64_t, double>>& tempo = {}) {
    ClockTake take;
    EXPECT_TRUE(r.m_rec.arm(LoopLength::Bars2));
    double own = 0.0;
    bool started = false;
    double bpm = 120.0;
    size_t next_tempo = 0;
    while (r.now() < until) {
        while (next_tempo < tempo.size() && tempo[next_tempo].first <= r.now()) {
            bpm = tempo[next_tempo++].second;
        }
        if (!started) {
            own = r.m_clock.block_info().beat_end();
            take.m_start = r.now();
            take.m_start_beat = own;
        }
        // One event every 64 samples: exact offsets after the pad's spread.
        for (uint32_t k = 0; k < r.m_block / 64; ++k) {
            const double b = own + (static_cast<double>(64 * k) * bpm / 60.0 / k_sr);
            const double x = std::fmod(b, 8.0) / 8.0;
            r.m_pad.touch(1, static_cast<float>(x), 0.5f);
        }
        started = true;
        r.step();
        own += static_cast<double>(r.m_block) * bpm / 60.0 / k_sr;
        if (take.m_finish == 0 && r.m_rec.snapshot().m_state != MotionState::Recording) {
            take.m_finish = r.now();
        }
    }
    r.m_pad.release(1);
    r.step();
    return take;
}

// Every index was written once, on the take's own clock: lane x[j] = j / N
// except near the take seam (start index), where the seam blend works.
void check_clock_lane(const MotionLane& lane, double start_beat, double tol) {
    ASSERT_FALSE(lane.empty());
    const size_t n = lane.num_points();
    const auto start_index = static_cast<size_t>(std::ceil(std::fmod(start_beat, 8.0) * n / 8.0));
    for (size_t j = 0; j < n; ++j) {
        const size_t from_start = (j + n - start_index) % n;
        if (from_start < 4 || from_start > n - 36) { continue; }  // take seam
        if (j < 3 || j > n - 3) { continue; }                     // x wraps 1 → 0 at index 0
        const double want = static_cast<double>(j) / static_cast<double>(n);
        ASSERT_NEAR(lane.m_x[j], want, tol) << j;
        ASSERT_EQ(lane.m_gate[j], 1) << j;
    }
}

}  // namespace

// Seek, DAW-loop wrap and Link realignment in the middle of a 2-bar take: the
// take is not aborted, writes all 8 · tpb indices once and ends after 8 beats of
// its own clock; playback re-locks to the transport afterwards.
TEST_P(MotionRecorderClock, JumpDuringBarRecording) {
    const uint32_t block = GetParam();
    auto rp = std::make_unique<Rig>(block, 120.0);
    Rig& r = *rp;
    r.play_at(0);
    r.run_until(k_spb + (k_spb / 3));
    const uint64_t t0 = r.now();
    r.seek_at(t0 + (2 * k_spb), 30.25);       // seek
    r.loop_at(t0 + (3 * k_spb), 30.0, 32.0);  // DAW loop that wraps during the take
    r.shift_at(t0 + (6 * k_spb), 0.37);       // Link realignment
    const ClockTake take = record_clock_take(r, t0 + (9 * k_spb));
    EXPECT_NEAR(static_cast<double>(take.m_finish - take.m_start) / k_spb,
                8.0,
                (block + 240.0 + 1.0) / k_spb);
    r.m_rec.service();
    const MotionLane lane = r.m_rec.lane();
    EXPECT_EQ(lane.num_points(), 800u);
    EXPECT_DOUBLE_EQ(lane.m_length, 8.0);
    check_clock_lane(lane, take.m_start_beat, 0.004);

    // Re-locked: after the release glide the output is the lane at the transport
    // beat (the DAW loop is switched off first; its wraps would glide).
    r.loop_off_at(r.now());
    r.step();
    const size_t from = r.m_x.size() + k_glide + 64;
    r.run_until(r.now() + (6 * k_spb));
    for (size_t s = from; s < r.m_x.size(); s += 53) {
        const MotionPoint p = lane.sample(std::fmod(r.m_beat[s], 8.0));
        ASSERT_NEAR(r.m_x[s], p.m_x, 2e-3) << s;
    }
    EXPECT_TRUE(motion_test::all_in_unit_range(r.m_x));
}

// 120 → 80 BPM in mid-take: the lane stays uniform in beats, and a replay at
// 100 BPM matches the input at the same beats.
TEST_P(MotionRecorderClock, TempoChangeDuringRecording) {
    const uint32_t block = GetParam();
    Rig r(block, 120.0);
    r.play_at(0);
    auto draw = [](double beat) {
        return std::pair<float, float>{
            static_cast<float>(0.5 + (0.35 * std::sin(2.0 * std::numbers::pi * beat / 4.0))),
            static_cast<float>(0.5 + (0.35 * std::cos(2.0 * std::numbers::pi * beat / 4.0)))};
    };
    r.run_until(k_spb);
    EXPECT_TRUE(r.m_rec.arm(LoopLength::Bars1));
    const uint64_t change = r.now() + (2 * k_spb);
    r.tempo_at(change, 80.0);
    const double start_beat = r.m_clock.block_info().beat_end();
    const uint64_t end = r.now() + (8 * k_spb);
    while (r.now() < end) {
        // The next block starts at beat_end(); SimHost applies the tempo change
        // at the first block start at or after `change`.
        const double next_bpm = r.now() >= change ? 80.0 : 120.0;
        const double begin = r.m_clock.block_info().beat_end();
        for (uint32_t k = 0; k < block / 64; ++k) {
            const auto [x, y] = draw(begin + (64.0 * k * next_bpm / 60.0 / k_sr));
            r.m_pad.touch(1, x, y);
        }
        r.step();
    }
    r.m_pad.release(1);
    r.m_rec.service();
    const MotionLane lane = r.m_rec.lane();
    ASSERT_FALSE(lane.empty());
    EXPECT_DOUBLE_EQ(lane.m_length, 4.0);
    // Uniform in beats: point j is the drawing at beat j / 100 (away from the take seam).
    const size_t n = lane.num_points();
    const auto seam = static_cast<size_t>(std::ceil(std::fmod(start_beat, 4.0) * 100.0)) % n;
    for (size_t j = 0; j < n; ++j) {
        const size_t from_seam = (j + n - seam) % n;
        if (from_seam < 4 || from_seam > n - 36) { continue; }
        const auto [x, y] = draw(static_cast<double>(j) / 100.0);
        EXPECT_NEAR(lane.m_x[j], x, 0.005) << j;
        EXPECT_NEAR(lane.m_y[j], y, 0.005) << j;
    }
    r.tempo_at(r.now(), 100.0);
    const size_t from = r.m_x.size() + k_glide + (2 * block);
    r.run_until(r.now() + (8 * k_spb));
    double max_err = 0.0;
    for (size_t s = from; s < r.m_x.size(); s += 11) {
        const auto [x, y] = draw(r.m_beat[s]);
        max_err = std::max(max_err, static_cast<double>(std::abs(r.m_x[s] - x)));
        max_err = std::max(max_err, static_cast<double>(std::abs(r.m_y[s] - y)));
    }
    EXPECT_LE(max_err, 0.005);
}

// The transport stops in mid-take: the take completes on its own clock at the
// last tempo.
TEST_P(MotionRecorderClock, StopDuringBarRecording) {
    const uint32_t block = GetParam();
    Rig r(block, 120.0);
    r.play_at(0);
    r.run_until(k_spb);
    r.stop_at(r.now() + (3 * k_spb));
    const ClockTake take = record_clock_take(r, r.now() + (9 * k_spb));
    ASSERT_GT(take.m_finish, 0u);
    EXPECT_NEAR(static_cast<double>(take.m_finish - take.m_start) / k_spb,
                8.0,
                (block + 240.0 + 1.0) / k_spb);
    r.m_rec.service();
    const MotionLane lane = r.m_rec.lane();
    EXPECT_EQ(lane.num_points(), 800u);
    check_clock_lane(lane, take.m_start_beat, 0.004);
}

// prepare() during a take aborts it, keeps the previous lane and reports it.
TEST_P(MotionRecorderClock, PrepareAbortsTake) {
    const uint32_t block = GetParam();
    auto rp = playing_rig(block);
    Rig& r = *rp;
    r.run_until(k_spb);
    EXPECT_TRUE(r.m_rec.arm(LoopLength::Bars1));
    r.m_pad.touch(1, 0.3f, 0.3f);
    r.run_until(2 * k_spb);
    ASSERT_EQ(r.m_rec.snapshot().m_state, MotionState::Recording);
    const MotionLane before = r.m_rec.lane();
    r.m_rec.prepare(k_sr, 4096);
    EXPECT_EQ(r.m_rec.lane().m_take_id, before.m_take_id);
    EXPECT_EQ(r.m_rec.lane().m_x, before.m_x);
    r.m_pad.release(1);
    r.run_until(3 * k_spb);
    EXPECT_NE(r.m_rec.snapshot().m_state, MotionState::Recording);
    EXPECT_TRUE(r.m_rec.snapshot().m_aborted);
    EXPECT_FALSE(r.m_rec.service());
    // Playback re-locked to the transport without a glide.
    EXPECT_EQ(r.m_rec.jump_glide_count(), 0u);
    EXPECT_LE(phase_error(r, 2 * k_spb + 64 + k_glide, r.m_x.size()), k_tick_beats);
}

// A DAW reports one bpm per block but its ppq follows the tempo map per sample
// (SimHost TempoAt / TempoRamp). The clock absorbs the difference as a tempo
// change, so a bar take stays on the bar grid and a playing lane never glides.
// Run at 64 .. 2048 samples (the error grows with the block).

namespace {

constexpr double k_draw_radius = 0.35;
// Lane vs drawing (beats): smoothing and the finger's 64-sample event grid, measured
// 0.0034 with the tempo changing at block boundaries; one tick is 0.01 beats.
constexpr double k_lane_offset_tol = 0.005;

std::pair<float, float> draw_circle(double beat) {
    const double ph = 2.0 * std::numbers::pi * beat / k_lane_beats;
    return {static_cast<float>(0.5 + (k_draw_radius * std::sin(ph))),
            static_cast<float>(0.5 + (k_draw_radius * std::cos(ph)))};
}

// Beat (mod one bar) that draw_circle() drew @p x, @p y at.
double beat_of_circle(float x, float y) {
    double t =
        std::atan2((x - 0.5) / k_draw_radius, (y - 0.5) / k_draw_radius) / (2.0 * std::numbers::pi);
    if (t < 0.0) { t += 1.0; }
    return t * k_lane_beats;
}

struct BarTakeResult {
    double m_lane_offset = 1e9;  // max |lane beat - drawn beat|
    // Distance of the finishing block from start + 1 bar. The last point is
    // written one tick before that (ticks start at the first one after the touch).
    double m_finish_error = 1e9;
    uint32_t m_jumps = 0;
};

// Record a 1-bar take whose finger follows the transport beat (one event per 64
// samples) while @p script changes the tempo, then compare the lane with the
// drawing at the same beat.
BarTakeResult bar_take_on_tempo_map(Rig& r) {
    BarTakeResult res;
    bool touching = false;
    r.m_before_pad = [&](const TransportInfo& t) {
        if (t.has(TransportInfo::k_jumped)) { ++res.m_jumps; }
        if (!touching) { return; }
        for (uint32_t k = 0; k < r.m_block / 64; ++k) {
            const auto [x, y] = draw_circle(t.beat_at(64 * k));
            r.m_pad.touch(1, x, y);
        }
    };
    r.run_until(k_spb * 2);
    EXPECT_TRUE(r.m_rec.arm(LoopLength::Bars1));
    touching = true;
    double start = -1.0;
    while (r.now() < 20 * k_spb) {
        const TransportInfo t = r.step();
        if (start < 0.0 && r.m_rec.snapshot().m_state == MotionState::Recording) {
            start = t.m_beat_position;  // the first touch is at offset 0
        }
        if (start >= 0.0 && r.m_rec.snapshot().m_state == MotionState::Playing) {
            // Finished inside this block: the bar ended in [start, end] of it.
            const double want = start + k_lane_beats;
            if (want < t.m_beat_position) {
                res.m_finish_error = t.m_beat_position - want;
            } else if (want > t.beat_end()) {
                res.m_finish_error = want - t.beat_end();
            } else {
                res.m_finish_error = 0.0;
            }
            break;
        }
    }
    touching = false;
    r.m_pad.release(1);
    r.step();
    r.m_rec.service();
    const MotionLane lane = r.m_rec.lane();
    EXPECT_EQ(lane.num_points(), static_cast<size_t>(k_lane_beats * lane.m_rate));
    double worst = 0.0;
    for (double b = 0.05; b < k_lane_beats; b += 0.01) {
        const MotionPoint p = lane.sample(b);
        worst = std::max(worst, circ(beat_of_circle(p.m_x, p.m_y), b, k_lane_beats));
    }
    res.m_lane_offset = worst;
    return res;
}

uint64_t into_block(uint32_t block, uint64_t around, uint32_t offset) {
    return (((around + block - 1) / block) * block) + offset;
}

}  // namespace

class MotionRecorderTempoMap : public ::testing::TestWithParam<uint32_t> {};

INSTANTIATE_TEST_SUITE_P(Blocks,
                         MotionRecorderTempoMap,
                         ::testing::Values(64u, 256u, 1024u, 2048u),
                         [](const auto& info) { return "Block" + std::to_string(info.param); });

// The tempo steps 120 → 180 (and 120 → 70) one sample into a block and in the
// middle of one, half a bar into a 1-bar take: the lane is on the bar grid and
// the take ends one bar after it started, in transport beats.
TEST_P(MotionRecorderTempoMap, TempoStepMidBlockDuringBarTake) {
    const uint32_t block = GetParam();
    for (const double to : {180.0, 70.0}) {
        for (const uint32_t offset : {1u, block / 2}) {
            SCOPED_TRACE(testing::Message() << "to " << to << " bpm, " << offset << " into");
            auto r = std::make_unique<Rig>(block);
            r->play_at(0);
            r->tempo_step_at(into_block(block, 4 * k_spb, offset), to);
            const BarTakeResult res = bar_take_on_tempo_map(*r);
            EXPECT_EQ(res.m_jumps, 0u);
            EXPECT_LE(res.m_lane_offset, k_lane_offset_tol);
            EXPECT_LE(res.m_finish_error, k_tick_beats + 1e-9);
        }
    }
}

// Ramps 120 → 60 and 120 → 180 across the whole take.
TEST_P(MotionRecorderTempoMap, TempoRampDuringBarTake) {
    const uint32_t block = GetParam();
    for (const double to : {60.0, 180.0}) {
        SCOPED_TRACE(testing::Message() << "to " << to << " bpm");
        auto r = std::make_unique<Rig>(block);
        r->play_at(0);
        r->tempo_ramp_at(3 * k_spb, to, 4 * k_spb);
        const BarTakeResult res = bar_take_on_tempo_map(*r);
        EXPECT_EQ(res.m_jumps, 0u);
        EXPECT_LE(res.m_lane_offset, k_lane_offset_tol);
        EXPECT_LE(res.m_finish_error, k_tick_beats + 1e-9);
    }
}

// A playing lane at a slow tempo (two lane points = 0.02 beats) through a step
// 60 → 120 inside a block and a ramp back to 60: no glide, the phase stays the
// transport's and the output has no step beyond the in-loop maximum.
TEST_P(MotionRecorderTempoMap, PlayingLaneFollowsTempoMapWithoutGlide) {
    const uint32_t block = GetParam();
    auto rp = playing_rig(block, 60.0);
    Rig& r = *rp;
    r.tempo_step_at(into_block(block, 4 * k_spb, block / 2), 120.0);
    r.tempo_ramp_at(into_block(block, 8 * k_spb, 7), 60.0, 4 * k_spb);
    uint32_t jumps = 0;
    r.m_before_pad = [&](const TransportInfo& t) {
        if (t.has(TransportInfo::k_jumped)) { ++jumps; }
    };
    r.run_until(16 * k_spb);
    EXPECT_EQ(jumps, 0u);
    EXPECT_EQ(r.m_rec.jump_glide_count(), 0u);
    EXPECT_LE(phase_error(r, 2 * k_glide, r.m_x.size()), k_tick_beats);
    check_output(r, 2 * k_glide, false);
}
