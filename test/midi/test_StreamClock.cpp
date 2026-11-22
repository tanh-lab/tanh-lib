// StreamClock places system time in the audio stream. These tests play audio callbacks of
// every unfriendly kind at it and check where known moments end up: the property the whole
// audio/MIDI alignment rests on.
#include <gtest/gtest.h>
#include <tanh/midi/StreamClock.h>

#include <cmath>
#include <cstdint>
#include <vector>

using thl::midi::StreamClock;

namespace {

constexpr double k_sample_rate = 48000.0;
constexpr double k_ns_per_frame = 1.0e9 / k_sample_rate;
constexpr std::uint32_t k_block = 480;  // 10 ms
constexpr std::int64_t k_block_ns = 10'000'000;
constexpr std::int64_t k_t0 = 5'000'000'000;  // some uptime

// One audio callback: when it is asked for, and how many frames.
struct Call {
    std::int64_t m_now;
    std::uint32_t m_frames;
};

// The time in the middle of frame `frame`, counted from `origin` at the nominal rate:
// away from the frame boundaries, so rounding cannot decide a test.
std::int64_t time_of_frame(double frame, std::int64_t origin = k_t0) {
    return origin + std::llround((frame + 0.5) * k_ns_per_frame);
}

// Runs the callbacks and returns, for every event time, the stream frame it was placed
// at (-1 if it never was). An event is only offered to blocks asked for after it happened.
std::vector<std::int64_t> place(StreamClock& clock,
                                const std::vector<Call>& calls,
                                const std::vector<std::int64_t>& events) {
    std::vector<std::int64_t> frames(events.size(), -1);
    std::int64_t position = 0;
    for (const Call& call : calls) {
        const StreamClock::Block block = clock.begin_block(call.m_now, call.m_frames);
        // What a block covers has already happened.
        EXPECT_LE(block.m_start_ns + std::llround(call.m_frames * k_ns_per_frame), call.m_now + 1);
        for (std::size_t i = 0; i < events.size(); ++i) {
            if (frames[i] >= 0 || events[i] > call.m_now) { continue; }
            std::uint32_t offset = 0;
            if (block.offset_of(events[i], offset)) { frames[i] = position + offset; }
        }
        position += call.m_frames;
    }
    return frames;
}

std::vector<Call> steady(int blocks) {
    std::vector<Call> calls;
    for (int k = 0; k < blocks; ++k) { calls.push_back({k_t0 + k * k_block_ns, k_block}); }
    return calls;
}

// Events every 7.37 ms from `first_frame` on: no relation to the block size.
std::vector<double> event_frames(double first_frame, int count) {
    std::vector<double> frames;
    for (int i = 0; i < count; ++i) { frames.push_back(std::floor(first_frame + i * 353.7)); }
    return frames;
}

std::vector<std::int64_t> times_of(const std::vector<double>& frames) {
    std::vector<std::int64_t> times;
    for (const double frame : frames) { times.push_back(time_of_frame(frame)); }
    return times;
}

}  // namespace

TEST(MidiStreamClock, OffsetOfABlock) {
    StreamClock clock;
    clock.prepare(k_sample_rate, k_block);
    const StreamClock::Block block = clock.begin_block(k_t0, k_block);
    EXPECT_EQ(block.m_frames, k_block);
    EXPECT_EQ(block.m_start_ns, k_t0 - k_block_ns);  // one block before the request

    std::uint32_t offset = 99;
    EXPECT_TRUE(block.offset_of(block.m_start_ns, offset));
    EXPECT_EQ(offset, 0U);
    EXPECT_TRUE(block.offset_of(time_of_frame(100, block.m_start_ns), offset));
    EXPECT_EQ(offset, 100U);
    EXPECT_TRUE(block.offset_of(time_of_frame(479, block.m_start_ns), offset));
    EXPECT_EQ(offset, 479U);
    // Before the block: at its start. After it: not due yet.
    EXPECT_TRUE(block.offset_of(block.m_start_ns - 1'000'000'000, offset));
    EXPECT_EQ(offset, 0U);
    EXPECT_FALSE(block.offset_of(time_of_frame(480, block.m_start_ns), offset));
}

// The reference case: callbacks exactly on time. Every event lands one block after the
// frame it happened at.
TEST(MidiStreamClock, SteadyCallbacksPlaceEventsToTheSample) {
    StreamClock clock;
    clock.prepare(k_sample_rate, k_block);
    const auto frames = event_frames(100, 250);
    const auto placed = place(clock, steady(200), times_of(frames));
    for (std::size_t i = 0; i < frames.size(); ++i) {
        EXPECT_EQ(placed[i], static_cast<std::int64_t>(frames[i]) + k_block) << "event " << i;
    }
    EXPECT_EQ(clock.latency_ns(), k_block_ns);
}

TEST(MidiStreamClock, ConfiguredLatencyShiftsEverythingAlike) {
    StreamClock clock;
    clock.prepare(k_sample_rate, k_block, 1'000'000);  // 1 ms = 48 frames
    const auto frames = event_frames(100, 250);
    const auto placed = place(clock, steady(200), times_of(frames));
    for (std::size_t i = 0; i < frames.size(); ++i) {
        EXPECT_EQ(placed[i], static_cast<std::int64_t>(frames[i]) + k_block + 48) << "event " << i;
    }
    EXPECT_EQ(clock.latency_ns(), k_block_ns + 1'000'000);
}

// Callbacks that come late by up to 4 ms, at random: the placement must not move by a
// single sample, as long as some callbacks are on time.
TEST(MidiStreamClock, LateCallbacksDoNotMoveEvents) {
    StreamClock clock;
    clock.prepare(k_sample_rate, k_block);
    std::vector<Call> calls = steady(200);
    std::uint32_t seed = 12345;
    for (std::size_t k = 0; k < calls.size(); ++k) {
        seed = seed * 1664525U + 1013904223U;
        if (k % 5 != 0) { calls[k].m_now += (seed >> 8U) % 4'000'000; }
    }
    const auto frames = event_frames(100, 250);
    const auto placed = place(clock, calls, times_of(frames));
    for (std::size_t i = 0; i < frames.size(); ++i) {
        EXPECT_EQ(placed[i], static_cast<std::int64_t>(frames[i]) + k_block) << "event " << i;
    }
}

// Android's habit: two blocks back to back, then twice the pause. The second block of
// each pair is asked for a whole block early; the clock takes that as the stream's time
// and every event is one more block late — but all by the same amount.
TEST(MidiStreamClock, BurstsOfCallbacksGiveAConstantDelay) {
    StreamClock clock;
    clock.prepare(k_sample_rate, k_block);
    std::vector<Call> calls;
    for (int k = 0; k < 100; ++k) {
        calls.push_back({k_t0 + 2 * k * k_block_ns, k_block});
        calls.push_back({k_t0 + 2 * k * k_block_ns + 50'000, k_block});
    }
    const auto frames = event_frames(2000, 220);
    const auto placed = place(clock, calls, times_of(frames));
    const std::int64_t delay = placed[0] - static_cast<std::int64_t>(frames[0]);
    EXPECT_GE(delay, 2 * k_block - 3);
    EXPECT_LE(delay, 2 * k_block);
    for (std::size_t i = 0; i < frames.size(); ++i) {
        EXPECT_EQ(placed[i] - static_cast<std::int64_t>(frames[i]), delay) << "event " << i;
    }
}

// A device period of 1000 frames handed on as chunks of 480, 480 and 40: blocks of
// different sizes. Their windows must join exactly, or events would bunch up at the
// chunk borders.
TEST(MidiStreamClock, ChunksOfDifferentSizesJoinWithoutGaps) {
    StreamClock clock;
    clock.prepare(k_sample_rate, k_block);
    std::vector<Call> calls;
    const std::int64_t period_ns = std::llround(1000 * k_ns_per_frame);
    for (int k = 0; k < 100; ++k) {
        const std::int64_t now = k_t0 + k * period_ns;
        calls.push_back({now, 480});
        calls.push_back({now, 480});
        calls.push_back({now, 40});
    }
    // An event on every third frame of a stretch: each must get a frame of its own.
    std::vector<double> frames;
    for (int i = 0; i < 2000; ++i) { frames.push_back(5000 + 3 * i); }
    const auto placed = place(clock, calls, times_of(frames));
    const std::int64_t delay = placed[0] - static_cast<std::int64_t>(frames[0]);
    for (std::size_t i = 0; i < frames.size(); ++i) {
        EXPECT_EQ(placed[i] - static_cast<std::int64_t>(frames[i]), delay) << "event " << i;
    }
}

// The audio clock and the system clock are different crystals. With the audio device
// 200 ppm slow or fast for a minute, nothing may pile up or drift away: the delay stays
// one block, give or take the few frames the estimate trails by.
TEST(MidiStreamClock, FollowsDriftBetweenAudioAndSystemClock) {
    for (const double drift : {200e-6, -200e-6}) {
        StreamClock clock;
        clock.prepare(k_sample_rate, k_block);
        std::vector<Call> calls;
        const double period = static_cast<double>(k_block_ns) * (1.0 + drift);
        for (int k = 0; k < 6000; ++k) {
            calls.push_back({k_t0 + std::llround(k * period), k_block});
        }

        // Events at known places of the *actual* stream: frame f happens at f / true rate.
        std::vector<double> frames;
        std::vector<std::int64_t> times;
        for (int i = 0; i < 4000; ++i) {
            const double frame = 1000.0 + i * 701.3;
            frames.push_back(frame);
            times.push_back(k_t0 + std::llround((frame + 0.5) * k_ns_per_frame * (1.0 + drift)));
        }
        const auto placed = place(clock, calls, times);
        std::int64_t previous = -1;
        for (std::size_t i = 0; i < frames.size(); ++i) {
            ASSERT_GE(placed[i], 0) << "event " << i << " was never placed";
            const double delay = static_cast<double>(placed[i]) - frames[i];
            EXPECT_GE(delay, k_block - 2.0) << "drift " << drift << ", event " << i;
            EXPECT_LE(delay, k_block + 8.0) << "drift " << drift << ", event " << i;
            EXPECT_GT(placed[i], previous);  // never out of order
            previous = placed[i];
        }
    }
}

// The app was suspended, or the device skipped: a quarter of a second without callbacks,
// after which the stream simply goes on. The clock takes up the new position at once.
TEST(MidiStreamClock, ResynchronisesAfterALongStall) {
    StreamClock clock;
    clock.prepare(k_sample_rate, k_block);
    const std::int64_t stall_ns = 250'000'000;
    std::vector<Call> calls;
    for (int k = 0; k < 300; ++k) {
        calls.push_back({k_t0 + k * k_block_ns + (k >= 100 ? stall_ns : 0), k_block});
    }
    // Events from 50 ms after the stream resumed. Stream frame f is asked for at
    // t0 + f + stall from there on.
    std::vector<std::int64_t> times;
    std::vector<double> frames;
    for (int i = 0; i < 150; ++i) {
        const double frame = 100.0 * k_block + 2400 + i * 353.7;
        frames.push_back(std::floor(frame));
        times.push_back(time_of_frame(std::floor(frame)) + stall_ns);
    }
    const auto placed = place(clock, calls, times);
    for (std::size_t i = 0; i < frames.size(); ++i) {
        EXPECT_EQ(placed[i], static_cast<std::int64_t>(frames[i]) + k_block) << "event " << i;
    }
}

// A shorter hiccup, below the resync threshold, is absorbed within half a second.
TEST(MidiStreamClock, RecoversFromAShortStall) {
    StreamClock clock;
    clock.prepare(k_sample_rate, k_block);
    const std::int64_t stall_ns = 30'000'000;
    std::vector<Call> calls;
    for (int k = 0; k < 300; ++k) {
        calls.push_back({k_t0 + k * k_block_ns + (k >= 100 ? stall_ns : 0), k_block});
    }
    std::vector<std::int64_t> times;
    std::vector<double> frames;
    for (int i = 0; i < 100; ++i) {
        const double frame = 100.0 * k_block + 0.6 * k_sample_rate + i * 353.7;
        frames.push_back(std::floor(frame));
        times.push_back(time_of_frame(std::floor(frame)) + stall_ns);
    }
    const auto placed = place(clock, calls, times);
    for (std::size_t i = 0; i < frames.size(); ++i) {
        EXPECT_EQ(placed[i], static_cast<std::int64_t>(frames[i]) + k_block) << "event " << i;
    }
}

TEST(MidiStreamClock, ResetStartsANewStream) {
    StreamClock clock;
    clock.prepare(k_sample_rate, k_block);
    (void)clock.begin_block(k_t0, k_block);
    (void)clock.begin_block(k_t0 + k_block_ns, k_block);
    clock.reset();
    // Hours later, a new stream: its first block is placed from its own request alone.
    const std::int64_t later = k_t0 + 3'600'000'000'000;
    const StreamClock::Block block = clock.begin_block(later, k_block);
    EXPECT_EQ(block.m_start_ns, later - k_block_ns);
}
