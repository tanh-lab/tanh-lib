// XYPad without a matrix: UI-side slot logic, queue coalescing and the
// audio-side render that an owner (XY controller) drives with process_block().

#include <gtest/gtest.h>
#include <tanh/modulation/XYPad.h>
#include <tanh/state/ModulationScope.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <thread>
#include <vector>

using namespace thl::modulation;

namespace {

constexpr uint32_t k_bs = 512;
constexpr ModulationScope k_voice_scope{.m_id = 1, .m_name = "voice"};

std::vector<uint32_t> cps(const XYPadStream& s) {
    return {s.m_change_points.begin(), s.m_change_points.end()};
}

XYPadConfig voice_config(uint32_t touches) {
    return XYPadConfig{.m_scope = k_voice_scope, .m_max_touches = touches};
}

}  // namespace

TEST(XYPad, DownMoveUpInOneBlock_SpreadsAndGates) {
    XYPad pad;
    pad.prepare(k_bs);
    ASSERT_TRUE(pad.touch(1, 0.1f, 0.2f));
    ASSERT_TRUE(pad.touch(1, 0.3f, 0.4f));
    ASSERT_TRUE(pad.release(1));
    pad.process_block(k_bs);

    const XYPadStream s = pad.stream(0);
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
    XYPad pad;
    pad.prepare(k_bs);
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(
            pad.touch(7, 0.1f * static_cast<float>(i + 1), 0.9f - 0.1f * static_cast<float>(i)));
    }
    pad.process_block(k_bs);
    const XYPadStream s = pad.stream(0);
    EXPECT_EQ(cps(s), (std::vector<uint32_t>{0, 102, 204, 307, 409}));
    for (uint32_t i = 1; i < k_bs; ++i) {
        EXPECT_EQ(s.m_x[i] != s.m_x[i - 1], s.m_y[i] != s.m_y[i - 1]) << "sample " << i;
    }
}

TEST(XYPad, HoldsAcrossEmptyBlocks) {
    XYPad pad;
    pad.prepare(k_bs);
    pad.touch(1, 0.25f, 0.75f);
    pad.process_block(k_bs);
    for (int b = 0; b < 3; ++b) {
        pad.process_block(k_bs);
        const XYPadStream s = pad.stream(0);
        EXPECT_TRUE(s.m_change_points.empty());
        EXPECT_FLOAT_EQ(s.m_x[0], 0.25f);
        EXPECT_FLOAT_EQ(s.m_y[k_bs - 1], 0.75f);
        EXPECT_EQ(s.m_active[0], 1);
    }
    pad.release(1);
    pad.process_block(100);  // shorter host block
    EXPECT_EQ(pad.stream(0).m_num_samples, 100u);
    EXPECT_EQ(cps(pad.stream(0)), (std::vector<uint32_t>{0}));
    pad.process_block(k_bs);
    EXPECT_EQ(pad.stream(0).m_active[k_bs - 1], 0);
    EXPECT_FLOAT_EQ(pad.stream(0).m_x[k_bs - 1], 0.25f);
}

TEST(XYPad, SurplusFingerRejected) {
    XYPad pad;  // max_touches = 1
    pad.prepare(k_bs);
    EXPECT_TRUE(pad.touch(1, 0.5f, 0.5f));
    EXPECT_FALSE(pad.touch(2, 0.1f, 0.1f));
    EXPECT_FALSE(pad.release(2));  // never claimed
    EXPECT_TRUE(pad.touch(1, 0.6f, 0.6f));
    EXPECT_TRUE(pad.release(1));
    EXPECT_TRUE(pad.touch(2, 0.1f, 0.1f));  // slot free again
    EXPECT_EQ(pad.touches().size(), 1u);
    EXPECT_EQ(pad.touches()[0].m_id, 2u);
}

TEST(XYPad, MonoLastPriority_FallsBackToHeldTouch) {
    XYPad pad(XYPadConfig{.m_max_touches = 3, .m_mono_priority = MonoPriority::Last});
    pad.prepare(k_bs);
    pad.touch(10, 0.1f, 0.1f);
    pad.touch(20, 0.5f, 0.5f);
    pad.touch(30, 0.9f, 0.9f);
    pad.process_block(k_bs);
    EXPECT_FLOAT_EQ(pad.stream(0).m_x[k_bs - 1], 0.9f);

    pad.touch(20, 0.6f, 0.6f);  // not driving: no output change
    pad.release(30);            // falls back to the newest remaining (20, latest position)
    pad.process_block(k_bs);
    EXPECT_FLOAT_EQ(pad.stream(0).m_x[k_bs - 1], 0.6f);
    EXPECT_EQ(pad.stream(0).m_active[k_bs - 1], 1);

    pad.release(20);
    pad.process_block(k_bs);
    EXPECT_FLOAT_EQ(pad.stream(0).m_x[k_bs - 1], 0.1f);
    EXPECT_EQ(pad.stream(0).m_active[k_bs - 1], 1);

    pad.release(10);
    pad.process_block(k_bs);
    EXPECT_EQ(pad.stream(0).m_active[k_bs - 1], 0);
    EXPECT_FLOAT_EQ(pad.stream(0).m_x[k_bs - 1], 0.1f);
}

TEST(XYPad, MonoFirstPriority) {
    XYPad pad(XYPadConfig{.m_max_touches = 2, .m_mono_priority = MonoPriority::First});
    pad.prepare(k_bs);
    pad.touch(1, 0.2f, 0.2f);
    pad.touch(2, 0.8f, 0.8f);
    pad.touch(2, 0.7f, 0.7f);
    pad.process_block(k_bs);
    EXPECT_EQ(cps(pad.stream(0)), (std::vector<uint32_t>{0}));  // only the first touch
    EXPECT_FLOAT_EQ(pad.stream(0).m_x[k_bs - 1], 0.2f);

    pad.release(1);
    pad.process_block(k_bs);
    EXPECT_FLOAT_EQ(pad.stream(0).m_x[k_bs - 1], 0.7f);
    EXPECT_EQ(pad.stream(0).m_active[k_bs - 1], 1);
    pad.release(2);
    pad.process_block(k_bs);
    EXPECT_EQ(pad.stream(0).m_active[k_bs - 1], 0);
}

TEST(XYPad, VoiceScope_ChordLandsAtZero) {
    XYPad pad(voice_config(4));
    pad.prepare(k_bs);
    EXPECT_EQ(pad.num_streams(), 4u);
    pad.touch(100, 0.1f, 0.1f);
    pad.touch(200, 0.2f, 0.2f);
    pad.touch(300, 0.3f, 0.3f);
    pad.process_block(k_bs);
    for (uint32_t v = 0; v < 3; ++v) {
        EXPECT_EQ(cps(pad.stream(v)), (std::vector<uint32_t>{0})) << "voice " << v;
        EXPECT_EQ(pad.stream(v).m_active[0], 1);
        EXPECT_FLOAT_EQ(pad.stream(v).m_x[0], 0.1f * static_cast<float>(v + 1));
    }
    EXPECT_EQ(pad.stream(3).m_active[0], 0);
    EXPECT_EQ(pad.primary_index(), 0u);

    pad.release(100);  // primary moves to the oldest remaining touch
    pad.process_block(k_bs);
    EXPECT_EQ(pad.primary_index(), 1u);
    EXPECT_EQ(pad.stream(0).m_active[0], 0);

    pad.touch(400, 0.4f, 0.4f);  // first free slot = 0
    pad.process_block(k_bs);
    EXPECT_FLOAT_EQ(pad.stream(0).m_x[0], 0.4f);
    EXPECT_EQ(pad.primary_index(), 1u);  // 200 is still older
}

TEST(XYPad, ReleaseAll_DropsEveryGate) {
    XYPad voice(voice_config(3));
    voice.prepare(k_bs);
    voice.touch(1, 0.1f, 0.1f);
    voice.touch(2, 0.2f, 0.2f);
    voice.process_block(k_bs);
    voice.release_all();
    voice.process_block(k_bs);
    for (uint32_t v = 0; v < 3; ++v) { EXPECT_EQ(voice.stream(v).m_active[0], 0); }
    for (const auto& t : voice.touches()) { EXPECT_FALSE(t.m_active); }

    XYPad mono(XYPadConfig{.m_max_touches = 3});
    mono.prepare(k_bs);
    mono.touch(1, 0.1f, 0.1f);
    mono.touch(2, 0.2f, 0.2f);
    mono.process_block(k_bs);
    mono.release_all();
    mono.process_block(k_bs);
    EXPECT_EQ(cps(mono.stream(0)), (std::vector<uint32_t>{0}));  // a single up, no fallback move
    EXPECT_EQ(mono.stream(0).m_active[0], 0);
    EXPECT_FLOAT_EQ(mono.stream(0).m_x[0], 0.2f);
}

TEST(XYPad, ClampsAndRejectsNaN) {
    XYPad pad;
    pad.prepare(k_bs);
    EXPECT_FALSE(pad.touch(1, std::numeric_limits<float>::quiet_NaN(), 0.5f));
    EXPECT_FALSE(pad.touch(1, 0.5f, std::numeric_limits<float>::infinity()));
    EXPECT_FALSE(pad.touches()[0].m_active);
    EXPECT_TRUE(pad.touch(1, -3.0f, 7.0f));
    pad.process_block(k_bs);
    EXPECT_FLOAT_EQ(pad.stream(0).m_x[0], 0.0f);
    EXPECT_FLOAT_EQ(pad.stream(0).m_y[0], 1.0f);
}

TEST(XYPad, QueueFull_NoLostEdges) {
    XYPad pad;
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
    EXPECT_EQ(pad.stream(0).m_active[k_bs - 1], 1);
    EXPECT_TRUE(pad.flush());  // latest move, then the up
    pad.process_block(k_bs);
    const XYPadStream s = pad.stream(0);
    EXPECT_FLOAT_EQ(s.m_x[k_bs - 1], last);
    EXPECT_FLOAT_EQ(s.m_y[k_bs - 1], 1.0f - last);
    EXPECT_EQ(s.m_active[0], 1);  // the coalesced move is still a live sample
    EXPECT_EQ(s.m_active[k_bs - 1], 0);
}

TEST(XYPad, QueueFull_ReleaseThenRetouchKeepsBothEdges) {
    XYPad pad;
    pad.prepare(k_bs);
    pad.touch(1, 0.5f, 0.5f);
    for (int i = 0; i < 300; ++i) { pad.touch(1, 0.5f, 0.5f); }  // fill
    pad.release(1);                                              // pending up
    EXPECT_TRUE(pad.touch(2, 0.9f, 0.9f));                       // pending down after the up
    pad.process_block(k_bs);
    pad.flush();
    pad.process_block(k_bs);
    const XYPadStream s = pad.stream(0);
    // move (pre), up, down: the gate must drop and rise again inside this block.
    bool saw_drop = false;
    for (uint32_t i = 1; i < k_bs; ++i) { saw_drop = saw_drop || s.m_active[i] == 0; }
    EXPECT_TRUE(saw_drop);
    EXPECT_EQ(s.m_active[k_bs - 1], 1);
    EXPECT_FLOAT_EQ(s.m_x[k_bs - 1], 0.9f);
}

TEST(XYPad, QueueFull_NewTouchClaimsNothing) {
    XYPad pad(voice_config(2));
    pad.prepare(k_bs);
    pad.touch(1, 0.5f, 0.5f);
    for (int i = 0; i < 300; ++i) { pad.touch(1, 0.4f, 0.4f); }
    EXPECT_FALSE(pad.touch(2, 0.1f, 0.1f));  // down cannot be queued
    EXPECT_FALSE(pad.touches()[1].m_active);
    pad.process_block(k_bs);
    EXPECT_TRUE(pad.touch(2, 0.1f, 0.1f));
}

TEST(XYPad, Concurrent_UiAndAudio) {
    XYPad pad(voice_config(4));
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

    for (uint32_t v = 0; v < 4; ++v) {
        EXPECT_EQ(pad.stream(v).m_active[63], 0) << "voice " << v;
        EXPECT_FLOAT_EQ(pad.stream(v).m_x[63], pad.touches()[v].m_x) << "voice " << v;
    }
}
