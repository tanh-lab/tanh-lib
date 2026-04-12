#include <tanh/transport/InternalTransportClock.h>
#include <tanh/transport/TransportClock.h>

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <random>

namespace {

constexpr double k_sample_rate = 48000.0;
constexpr double k_epsilon = 1e-9;

// Tick one block through the clock
void tick(thl::InternalTransportClock& clk, uint32_t frames = 512) {
    clk.begin_block(frames);
    clk.end_block();
}

// ── Initial state ─────────────────────────────────────────────────────────────

TEST(InternalTransportClock, InitialState) {
    thl::InternalTransportClock clk;
    clk.prepare(k_sample_rate);
    clk.begin_block(512);

    EXPECT_FALSE(clk.is_playing());
    EXPECT_NEAR(clk.beat_at_sample(0), 0.0, k_epsilon);
    EXPECT_EQ(clk.sample_position(), 0u);

    clk.end_block();
}

// ── Stopped clock does not advance ───────────────────────────────────────────

TEST(InternalTransportClock, StoppedPositionDoesNotAdvance) {
    thl::InternalTransportClock clk;
    clk.prepare(k_sample_rate);

    tick(clk);
    tick(clk);

    clk.begin_block(512);
    EXPECT_NEAR(clk.beat_at_sample(0), 0.0, k_epsilon);
    clk.end_block();
}

// ── Beat advances while playing ───────────────────────────────────────────────

TEST(InternalTransportClock, BeatAdvancesWhilePlaying) {
    thl::InternalTransportClock clk;
    clk.prepare(k_sample_rate);

    // At 120 BPM: one beat = 24000 samples
    constexpr auto k_frames_per_beat = static_cast<uint32_t>(k_sample_rate * 60.0 / 120.0);

    clk.play();
    tick(clk, k_frames_per_beat);

    clk.begin_block(1);
    EXPECT_NEAR(clk.beat_at_sample(0), 1.0, 1e-6);
    clk.end_block();
}

// ── beat_at_sample interpolates within a block ────────────────────────────────

TEST(InternalTransportClock, BeatAtSampleInterpolates) {
    thl::InternalTransportClock clk;
    clk.prepare(k_sample_rate);
    clk.play();

    const double bps = 120.0 / (60.0 * k_sample_rate);

    clk.begin_block(512);
    EXPECT_NEAR(clk.beat_at_sample(0), 0.0, 1e-12);
    EXPECT_NEAR(clk.beat_at_sample(256), 256.0 * bps, 1e-12);
    EXPECT_NEAR(clk.beat_at_sample(511), 511.0 * bps, 1e-12);
    clk.end_block();
}

// ── Stopped: beat_at_sample is constant across offsets ───────────────────────

TEST(InternalTransportClock, StoppedBeatAtSampleIsConstant) {
    thl::InternalTransportClock clk;
    clk.prepare(k_sample_rate);

    clk.begin_block(512);
    EXPECT_EQ(clk.beat_at_sample(0), clk.beat_at_sample(256));
    clk.end_block();
}

// ── Seek ──────────────────────────────────────────────────────────────────────

TEST(InternalTransportClock, SeekTakesEffectNextBlock) {
    thl::InternalTransportClock clk;
    clk.prepare(k_sample_rate);

    clk.set_position_beats(4.0);
    clk.begin_block(512);
    EXPECT_NEAR(clk.beat_at_sample(0), 4.0, 1e-6);
    clk.end_block();
}

TEST(InternalTransportClock, SeekWhilePlaying) {
    thl::InternalTransportClock clk;
    clk.prepare(k_sample_rate);
    clk.play();

    constexpr auto k_frames_per_beat = static_cast<uint32_t>(k_sample_rate * 60.0 / 120.0);
    tick(clk, k_frames_per_beat);

    clk.set_position_beats(2.0);
    clk.begin_block(512);
    EXPECT_NEAR(clk.beat_at_sample(0), 2.0, 1e-6);
    clk.end_block();
}

// ── BPM change ────────────────────────────────────────────────────────────────

TEST(InternalTransportClock, BpmChangeTakesEffectNextBlock) {
    thl::InternalTransportClock clk;
    clk.prepare(k_sample_rate);
    clk.play();

    clk.set_bpm(240.0);

    // At 240 BPM: one beat = 12000 samples
    constexpr auto k_frames_per_beat_240 = static_cast<uint32_t>(k_sample_rate * 60.0 / 240.0);
    tick(clk, k_frames_per_beat_240);

    clk.begin_block(1);
    EXPECT_NEAR(clk.beat_at_sample(0), 1.0, 1e-6);
    clk.end_block();
}

// ── Play / stop cycle ─────────────────────────────────────────────────────────

TEST(InternalTransportClock, PlayStopCycle) {
    thl::InternalTransportClock clk;
    clk.prepare(k_sample_rate);

    clk.play();
    constexpr auto k_half_beat = static_cast<uint32_t>(k_sample_rate * 60.0 / 120.0 / 2.0);
    tick(clk, k_half_beat);

    clk.stop();
    tick(clk, k_half_beat);  // stopped — should not advance

    clk.begin_block(1);
    EXPECT_NEAR(clk.beat_at_sample(0), 0.5, 1e-6);
    clk.end_block();
}

// ── division_in_block ─────────────────────────────────────────────────────────

TEST(InternalTransportClock, DivisionInBlockBeatCrossing) {
    thl::InternalTransportClock clk;
    clk.prepare(k_sample_rate);
    clk.play();

    // Advance to one sample before beat 1, then check a 2-sample block
    tick(clk, 23999);

    clk.begin_block(2);
    EXPECT_TRUE(clk.division_in_block(thl::Division::Beat));
    clk.end_block();
}

TEST(InternalTransportClock, DivisionInBlockNoCrossing) {
    thl::InternalTransportClock clk;
    clk.prepare(k_sample_rate);
    clk.play();

    // Block entirely within the first beat
    clk.begin_block(512);
    EXPECT_FALSE(clk.division_in_block(thl::Division::Beat));
    clk.end_block();
}

TEST(InternalTransportClock, DivisionInBlockStoppedNeverFires) {
    thl::InternalTransportClock clk;
    clk.prepare(k_sample_rate);

    clk.begin_block(48000);
    EXPECT_FALSE(clk.division_in_block(thl::Division::Beat));
    clk.end_block();
}

TEST(InternalTransportClock, DivisionInBlockSixteenth) {
    thl::InternalTransportClock clk;
    clk.prepare(k_sample_rate);
    clk.play();

    // At 120 BPM a 16th note = 6000 samples
    tick(clk, 5999);

    clk.begin_block(2);
    EXPECT_TRUE(clk.division_in_block(thl::Division::Sixteenth));
    clk.end_block();
}

TEST(InternalTransportClock, DivisionInBlockBar) {
    thl::InternalTransportClock clk;
    clk.prepare(k_sample_rate);
    clk.set_time_signature(4, 4);
    clk.play();

    // At 120 BPM a bar (4 beats) = 96000 samples
    tick(clk, 95999);

    clk.begin_block(2);
    EXPECT_TRUE(clk.division_in_block(thl::Division::Bar));
    clk.end_block();
}

// ── Variable block size stress test ──────────────────────────────────────────
//
// Simulates miniaudio delivering random block sizes (1–512 samples) over
// 10 000 blocks. For every block we independently compute the expected beat
// position and division crossings from the raw sample counter and assert that
// the clock matches exactly. No tolerance is needed: both sides use identical
// integer arithmetic so the results must be bit-exact.

TEST(InternalTransportClock, VariableBlockSizeAccuracy) {
    constexpr double k_bpm = 120.0;
    constexpr double k_bps = k_bpm / (60.0 * k_sample_rate);
    constexpr int k_num_blocks = 10000;

    thl::InternalTransportClock clk;
    clk.prepare(k_sample_rate);
    clk.play();

    std::mt19937 rng(42);  // fixed seed — deterministic
    std::uniform_int_distribution<uint32_t> dist(1, 512);

    uint64_t total_samples = 0;

    for (int i = 0; i < k_num_blocks; ++i) {
        const uint32_t frame_count = dist(rng);

        clk.begin_block(frame_count);

        // ── beat position at block start ──────────────────────────────────
        const double expected_beat = static_cast<double>(total_samples) * k_bps;
        EXPECT_DOUBLE_EQ(clk.beat_at_sample(0), expected_beat)
            << "block " << i << ", frame_count=" << frame_count;

        // ── beat position at last sample of block ─────────────────────────
        const double expected_beat_last =
            static_cast<double>(total_samples + frame_count - 1) * k_bps;
        EXPECT_DOUBLE_EQ(clk.beat_at_sample(frame_count - 1), expected_beat_last)
            << "block " << i << ", frame_count=" << frame_count;

        // ── division_in_block must match independent floor check ──────────
        const double beat_end = static_cast<double>(total_samples + frame_count) * k_bps;

        auto expect_division = [&](thl::Division div, double size) {
            const bool expected = std::floor(beat_end / size) > std::floor(expected_beat / size);
            EXPECT_EQ(clk.division_in_block(div), expected)
                << "block " << i << ", frame_count=" << frame_count << ", division_size=" << size;
        };

        expect_division(thl::Division::Sixteenth, 0.25);
        expect_division(thl::Division::Eighth, 0.5);
        expect_division(thl::Division::Beat, 1.0);
        expect_division(thl::Division::Half, 2.0);
        expect_division(thl::Division::Bar, 4.0);

        clk.end_block();
        total_samples += frame_count;
    }
}

}  // namespace
