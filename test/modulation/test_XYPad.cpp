#include <gtest/gtest.h>
#include <tanh/modulation/MotionRecorder.h>
#include <tanh/modulation/XYController.h>
#include <tanh/modulation/detail/XYPad.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <thread>
#include <vector>

using namespace thl::modulation;
using thl::modulation::detail::XYPad;

namespace {

constexpr uint32_t k_bs = 512;

std::vector<uint32_t> cps(const MotionInput& s) {
    return {s.m_change_points.begin(), s.m_change_points.end()};
}

}  // namespace

TEST(XYPad, DownMoveUpInOneBlock_SpreadsAndGates) {
    XYPad pad(1, MonoPriority::Last);
    pad.prepare(k_bs);
    ASSERT_TRUE(pad.touch(1, 0.1f, 0.2f));
    ASSERT_TRUE(pad.touch(1, 0.3f, 0.4f));
    ASSERT_TRUE(pad.release(1));
    pad.process_block(k_bs);

    const MotionInput s = pad.output();
    EXPECT_EQ(s.m_num_samples, k_bs);
    EXPECT_EQ(cps(s), (std::vector<uint32_t>{0, 170, 341}));
    EXPECT_FLOAT_EQ(s.m_x[0], 0.1f);
    EXPECT_FLOAT_EQ(s.m_y[169], 0.2f);
    EXPECT_FLOAT_EQ(s.m_x[170], 0.3f);
    EXPECT_FLOAT_EQ(s.m_y[340], 0.4f);
    EXPECT_FLOAT_EQ(s.m_x[511], 0.3f);  // up keeps the position
    EXPECT_EQ(s.m_active[0], 1);
    EXPECT_EQ(s.m_active[340], 1);
    EXPECT_EQ(s.m_active[341], 0);
    EXPECT_EQ(s.m_active[511], 0);
}

TEST(XYPad, XAndYChangeAtSameOffsets) {
    XYPad pad(1, MonoPriority::Last);
    pad.prepare(k_bs);
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(
            pad.touch(7, 0.1f * static_cast<float>(i + 1), 0.9f - 0.1f * static_cast<float>(i)));
    }
    pad.process_block(k_bs);
    const MotionInput s = pad.output();
    EXPECT_EQ(cps(s), (std::vector<uint32_t>{0, 102, 204, 307, 409}));
    for (uint32_t i = 1; i < k_bs; ++i) {
        EXPECT_EQ(s.m_x[i] != s.m_x[i - 1], s.m_y[i] != s.m_y[i - 1]) << "sample " << i;
    }
}

TEST(XYPad, HoldsAcrossEmptyBlocks) {
    XYPad pad(1, MonoPriority::Last);
    pad.prepare(k_bs);
    pad.touch(1, 0.25f, 0.75f);
    pad.process_block(k_bs);
    for (int b = 0; b < 3; ++b) {
        pad.process_block(k_bs);
        const MotionInput s = pad.output();
        EXPECT_TRUE(s.m_change_points.empty());
        EXPECT_FLOAT_EQ(s.m_x[0], 0.25f);
        EXPECT_FLOAT_EQ(s.m_y[k_bs - 1], 0.75f);
        EXPECT_EQ(s.m_active[0], 1);
    }
    pad.release(1);
    pad.process_block(100);  // shorter host block
    EXPECT_EQ(pad.output().m_num_samples, 100u);
    EXPECT_EQ(cps(pad.output()), (std::vector<uint32_t>{0}));
    pad.process_block(k_bs);
    EXPECT_EQ(pad.output().m_active[k_bs - 1], 0);
    EXPECT_FLOAT_EQ(pad.output().m_x[k_bs - 1], 0.25f);
}

TEST(XYPad, SurplusFingerRejected) {
    XYPad pad(1, MonoPriority::Last);
    pad.prepare(k_bs);
    EXPECT_TRUE(pad.touch(1, 0.5f, 0.5f));
    EXPECT_FALSE(pad.touch(2, 0.1f, 0.1f));
    EXPECT_FALSE(pad.release(2));  // never claimed
    EXPECT_TRUE(pad.touch(1, 0.6f, 0.6f));
    EXPECT_TRUE(pad.release(1));
    EXPECT_TRUE(pad.touch(2, 0.1f, 0.1f));  // slot free again
    EXPECT_TRUE(pad.release(2));
}

TEST(XYPad, MonoLastPriority_FallsBackToHeldTouch) {
    XYPad pad(3, MonoPriority::Last);
    pad.prepare(k_bs);
    pad.touch(10, 0.1f, 0.1f);
    pad.touch(20, 0.5f, 0.5f);
    pad.touch(30, 0.9f, 0.9f);
    pad.process_block(k_bs);
    EXPECT_FLOAT_EQ(pad.output().m_x[k_bs - 1], 0.9f);

    pad.touch(20, 0.6f, 0.6f);  // not driving: no output change
    pad.release(30);            // falls back to the newest remaining (20, latest position)
    pad.process_block(k_bs);
    EXPECT_FLOAT_EQ(pad.output().m_x[k_bs - 1], 0.6f);
    EXPECT_EQ(pad.output().m_active[k_bs - 1], 1);

    pad.release(20);
    pad.process_block(k_bs);
    EXPECT_FLOAT_EQ(pad.output().m_x[k_bs - 1], 0.1f);
    EXPECT_EQ(pad.output().m_active[k_bs - 1], 1);

    pad.release(10);
    pad.process_block(k_bs);
    EXPECT_EQ(pad.output().m_active[k_bs - 1], 0);
    EXPECT_FLOAT_EQ(pad.output().m_x[k_bs - 1], 0.1f);
}

TEST(XYPad, MonoFirstPriority) {
    XYPad pad(2, MonoPriority::First);
    pad.prepare(k_bs);
    pad.touch(1, 0.2f, 0.2f);
    pad.touch(2, 0.8f, 0.8f);
    pad.touch(2, 0.7f, 0.7f);
    pad.process_block(k_bs);
    EXPECT_EQ(cps(pad.output()), (std::vector<uint32_t>{0}));  // only the first touch
    EXPECT_FLOAT_EQ(pad.output().m_x[k_bs - 1], 0.2f);

    pad.release(1);
    pad.process_block(k_bs);
    EXPECT_FLOAT_EQ(pad.output().m_x[k_bs - 1], 0.7f);
    EXPECT_EQ(pad.output().m_active[k_bs - 1], 1);
    pad.release(2);
    pad.process_block(k_bs);
    EXPECT_EQ(pad.output().m_active[k_bs - 1], 0);
}

TEST(XYPad, ReleaseAll_SendsOneUp) {
    XYPad mono(3, MonoPriority::Last);
    mono.prepare(k_bs);
    mono.touch(1, 0.1f, 0.1f);
    mono.touch(2, 0.2f, 0.2f);
    mono.process_block(k_bs);
    mono.release_all();
    mono.process_block(k_bs);
    EXPECT_EQ(cps(mono.output()), (std::vector<uint32_t>{0}));  // a single up, no fallback move
    EXPECT_EQ(mono.output().m_active[0], 0);
    EXPECT_FLOAT_EQ(mono.output().m_x[0], 0.2f);
}

TEST(XYPad, ClampsAndRejectsNaN) {
    XYPad pad(1, MonoPriority::Last);
    pad.prepare(k_bs);
    EXPECT_FALSE(pad.touch(1, std::numeric_limits<float>::quiet_NaN(), 0.5f));
    EXPECT_FALSE(pad.touch(1, 0.5f, std::numeric_limits<float>::infinity()));
    EXPECT_FALSE(pad.release(1));
    EXPECT_TRUE(pad.touch(1, -3.0f, 7.0f));
    pad.process_block(k_bs);
    EXPECT_FLOAT_EQ(pad.output().m_x[0], 0.0f);
    EXPECT_FLOAT_EQ(pad.output().m_y[0], 1.0f);
}

TEST(XYPad, QueueFull_NoLostEdges) {
    XYPad pad(1, MonoPriority::Last);
    pad.prepare(k_bs);
    ASSERT_TRUE(pad.touch(1, 0.0f, 0.0f));
    float last = 0.0f;
    for (int i = 1; i <= 10000; ++i) {
        last = static_cast<float>(i) / 10000.0f;
        ASSERT_TRUE(pad.touch(1, last, 1.0f - last));  // coalesced once the queue is full
    }
    ASSERT_TRUE(pad.release(1));  // cannot be queued yet → pending
    EXPECT_FALSE(pad.flush());

    pad.process_block(k_bs);  // drains the full queue (down + 255 moves)
    EXPECT_EQ(pad.output().m_active[k_bs - 1], 1);
    EXPECT_TRUE(pad.flush());  // latest move, then the up
    pad.process_block(k_bs);
    const MotionInput s = pad.output();
    EXPECT_FLOAT_EQ(s.m_x[k_bs - 1], last);
    EXPECT_FLOAT_EQ(s.m_y[k_bs - 1], 1.0f - last);
    EXPECT_EQ(s.m_active[0], 1);  // the coalesced move is still a live sample
    EXPECT_EQ(s.m_active[k_bs - 1], 0);
}

TEST(XYPad, QueueFull_ReleaseThenRetouchKeepsBothEdges) {
    XYPad pad(1, MonoPriority::Last);
    pad.prepare(k_bs);
    pad.touch(1, 0.5f, 0.5f);
    for (int i = 0; i < 300; ++i) { pad.touch(1, 0.5f, 0.5f); }  // fill
    pad.release(1);                                              // pending up
    EXPECT_TRUE(pad.touch(2, 0.9f, 0.9f));                       // pending down after the up
    pad.process_block(k_bs);
    pad.flush();
    pad.process_block(k_bs);
    const MotionInput s = pad.output();
    // move (pre), up, down: the gate must drop and rise again inside this block.
    bool saw_drop = false;
    for (uint32_t i = 1; i < k_bs; ++i) { saw_drop = saw_drop || s.m_active[i] == 0; }
    EXPECT_TRUE(saw_drop);
    EXPECT_EQ(s.m_active[k_bs - 1], 1);
    EXPECT_FLOAT_EQ(s.m_x[k_bs - 1], 0.9f);
}

TEST(XYPad, QueueFull_NewTouchClaimsNothing) {
    XYPad pad(1, MonoPriority::Last);
    pad.prepare(k_bs);
    // 128 taps fill the queue exactly, with nothing pending.
    for (int i = 0; i < 128; ++i) {
        ASSERT_TRUE(pad.touch(1, 0.4f, 0.4f));
        ASSERT_TRUE(pad.release(1));
    }
    EXPECT_FALSE(pad.touch(2, 0.1f, 0.1f));  // the down cannot be queued
    EXPECT_FALSE(pad.release(2));            // so the touch claimed no slot
    pad.process_block(k_bs);
    EXPECT_TRUE(pad.touch(2, 0.1f, 0.1f));
}

TEST(XYPad, Concurrent_UiAndAudio) {
    XYPad pad(4, MonoPriority::Last);
    pad.prepare(64);
    std::atomic<bool> done{false};

    std::thread audio([&] {
        while (!done.load(std::memory_order_acquire)) { pad.process_block(64); }
        for (int i = 0; i < 4; ++i) { pad.process_block(64); }
    });

    std::mt19937 rng(5);
    std::uniform_int_distribution<int> op(0, 9);
    std::uniform_int_distribution<TouchId> id(1, 6);
    std::uniform_real_distribution<float> pos(0.0f, 1.0f);
    for (int i = 0; i < 100000; ++i) {
        const int o = op(rng);
        if (o < 7) {
            pad.touch(id(rng), pos(rng), pos(rng));
        } else if (o < 9) {
            pad.release(id(rng));
        } else {
            pad.flush();
        }
    }
    pad.release_all();
    while (!pad.flush()) { std::this_thread::yield(); }
    done.store(true, std::memory_order_release);
    audio.join();

    const MotionInput s = pad.output();
    EXPECT_EQ(s.m_active[63], 0);
    EXPECT_GE(s.m_x[63], 0.0f);
    EXPECT_LE(s.m_x[63], 1.0f);
}

namespace {

constexpr uint32_t k_timed_bs = 480;  // 10 ms at 48 kHz
constexpr int64_t k_ms = 1'000'000;
constexpr int64_t k_clock_start = 1'000'000'000;

/// A pad that plays timestamps 20 ms late.
struct TimedPad : XYPad {
    TimedPad() : XYPad(2, MonoPriority::Last) {
        prepare(k_timed_bs, {.m_sample_rate = 48000.0, .m_delay_ns = 20 * k_ms});
    }
};

int64_t block_time(int block) {
    return k_clock_start + (block * 10 * k_ms);
}

}  // namespace

TEST(XYPad, TimedTouch_LandsAtItsSampleWhenQueuedEarlyOrLate) {
    const int64_t when = block_time(1) - (15 * k_ms);  // 5 ms into block 1 after the delay
    TimedPad early;
    TimedPad late;
    early.touch(1, 0.5f, 0.5f, when);
    early.process_block(k_timed_bs, block_time(0));  // due after this block: waits
    EXPECT_TRUE(early.output().m_change_points.empty());
    EXPECT_EQ(early.output().m_active[k_timed_bs - 1], 0);
    late.process_block(k_timed_bs, block_time(0));
    late.touch(1, 0.5f, 0.5f, when);

    for (XYPad* pad : {&early, &late}) {
        pad->process_block(k_timed_bs, block_time(1));
        const MotionInput s = pad->output();
        EXPECT_EQ(cps(s), (std::vector<uint32_t>{240}));
        EXPECT_EQ(s.m_active[239], 0);
        EXPECT_EQ(s.m_active[240], 1);
        EXPECT_FLOAT_EQ(s.m_x[240], 0.5f);
    }
}

TEST(XYPad, TimedTouch_LateLandsAtZeroAndFutureWaits) {
    TimedPad pad;
    pad.touch(1, 0.3f, 0.3f, block_time(0) - (100 * k_ms));  // far too late
    pad.touch(1, 0.4f, 0.4f, block_time(0) + (5 * k_ms));    // due in block 2
    pad.process_block(k_timed_bs, block_time(0));
    EXPECT_EQ(cps(pad.output()).size(), k_timed_bs / 32);  // a mark every 32 samples
    EXPECT_EQ(pad.output().m_active[0], 1);
    EXPECT_FLOAT_EQ(pad.output().m_x[0], 0.3f);

    pad.process_block(k_timed_bs, block_time(1));
    pad.process_block(k_timed_bs, block_time(2));
    EXPECT_FLOAT_EQ(pad.output().m_x[239], 0.4f - (0.1f / 1200.0f));
    EXPECT_FLOAT_EQ(pad.output().m_x[240], 0.4f);
}

TEST(XYPad, TimedMoves_RampLinearlyWithSharpEdges) {
    TimedPad pad;
    const int64_t down = block_time(0) - (19 * k_ms);  // sample 48
    pad.touch(1, 0.2f, 0.8f, down);
    pad.touch(1, 0.6f, 0.4f, down + (2 * k_ms));  // sample 144
    pad.release(1, down + (4 * k_ms));            // sample 240
    pad.process_block(k_timed_bs, block_time(0));

    const MotionInput s = pad.output();
    EXPECT_EQ(cps(s), (std::vector<uint32_t>{48, 80, 112, 144, 240}));
    EXPECT_EQ(s.m_active[47], 0);
    EXPECT_FLOAT_EQ(s.m_x[47], 0.0f);  // the down does not ramp in
    EXPECT_EQ(s.m_active[48], 1);
    EXPECT_FLOAT_EQ(s.m_x[48], 0.2f);
    EXPECT_FLOAT_EQ(s.m_x[96], 0.4f);
    EXPECT_FLOAT_EQ(s.m_y[96], 0.6f);
    EXPECT_FLOAT_EQ(s.m_x[144], 0.6f);
    EXPECT_FLOAT_EQ(s.m_x[239], 0.6f);  // no ramp into the release
    EXPECT_EQ(s.m_active[239], 1);
    EXPECT_EQ(s.m_active[240], 0);
}

TEST(XYPad, TimedTouches_WithoutBlockClockSpreadLikeUntimed) {
    TimedPad timed;
    TimedPad untimed;
    for (int i = 0; i < 4; ++i) {
        const float v = 0.2f * static_cast<float>(i + 1);
        timed.touch(1, v, v, block_time(0) + (i * k_ms));
        untimed.touch(1, v, v);
    }
    timed.process_block(k_timed_bs);
    untimed.process_block(k_timed_bs);
    EXPECT_EQ(cps(timed.output()), cps(untimed.output()));
    for (uint32_t i = 0; i < k_timed_bs; ++i) {
        ASSERT_EQ(timed.output().m_x[i], untimed.output().m_x[i]) << i;
    }
}
