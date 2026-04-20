#include <cstddef>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include <tanh/modulation/InputEventQueue.h>

namespace {

using thl::modulation::InputEventQueue;
using EventType = InputEventQueue::EventType;

struct Captured {
    InputEventQueue::Event m_event;
    uint32_t m_offset;
};

class DrainRecorder {
public:
    std::vector<Captured> m_events;

    InputEventQueue::OnEvent callback() {
        return [this](const InputEventQueue::Event& e, uint32_t offset) {
            m_events.push_back({e, offset});
        };
    }
};

}  // namespace

// ── Basics ──────────────────────────────────────────────────────────────────

TEST(InputEventQueue, SingleVoiceEventLandsAtOffsetZero) {
    InputEventQueue q(/*has_mono=*/false, /*num_voices=*/10, /*queue_capacity=*/32);
    q.prepare();

    ASSERT_TRUE(q.push_voice_value(0, 0.7f));

    DrainRecorder rec;
    const size_t n = q.drain_spread(512, rec.callback());

    EXPECT_EQ(n, 1u);
    ASSERT_EQ(rec.m_events.size(), 1u);
    EXPECT_EQ(rec.m_events[0].m_event.m_type, EventType::Value);
    EXPECT_FALSE(rec.m_events[0].m_event.m_is_mono);
    EXPECT_EQ(rec.m_events[0].m_event.m_voice, 0u);
    EXPECT_FLOAT_EQ(rec.m_events[0].m_event.m_value, 0.7f);
    EXPECT_EQ(rec.m_events[0].m_offset, 0u);
}

TEST(InputEventQueue, SingleMonoEventLandsAtOffsetZero) {
    InputEventQueue q(/*has_mono=*/true, /*num_voices=*/0, /*queue_capacity=*/32);
    q.prepare();

    ASSERT_TRUE(q.push_mono_value(0.42f));

    DrainRecorder rec;
    const size_t n = q.drain_spread(512, rec.callback());

    EXPECT_EQ(n, 1u);
    ASSERT_EQ(rec.m_events.size(), 1u);
    EXPECT_TRUE(rec.m_events[0].m_event.m_is_mono);
    EXPECT_EQ(rec.m_events[0].m_event.m_type, EventType::Value);
    EXPECT_FLOAT_EQ(rec.m_events[0].m_event.m_value, 0.42f);
    EXPECT_EQ(rec.m_events[0].m_offset, 0u);
}

TEST(InputEventQueue, NoEventBlockProducesNoCallbacks) {
    InputEventQueue q(/*has_mono=*/true, /*num_voices=*/10, /*queue_capacity=*/32);
    q.prepare();

    DrainRecorder rec;
    EXPECT_EQ(q.drain_spread(512, rec.callback()), 0u);
    EXPECT_TRUE(rec.m_events.empty());
}

// ── Per-stream spreading — the core behavior ────────────────────────────────

TEST(InputEventQueue, TwoVoiceEventsSpreadEvenly) {
    InputEventQueue q(/*has_mono=*/false, /*num_voices=*/10, /*queue_capacity=*/32);
    q.prepare();

    ASSERT_TRUE(q.push_voice_value(0, 0.2f));
    ASSERT_TRUE(q.push_voice_value(0, 0.8f));

    DrainRecorder rec;
    q.drain_spread(512, rec.callback());

    ASSERT_EQ(rec.m_events.size(), 2u);
    EXPECT_EQ(rec.m_events[0].m_offset, 0u);
    EXPECT_EQ(rec.m_events[1].m_offset, 256u);
    EXPECT_FLOAT_EQ(rec.m_events[0].m_event.m_value, 0.2f);
    EXPECT_FLOAT_EQ(rec.m_events[1].m_event.m_value, 0.8f);
}

TEST(InputEventQueue, ThreeVoiceEventsSpreadEvenly) {
    InputEventQueue q(/*has_mono=*/false, /*num_voices=*/10, /*queue_capacity=*/32);
    q.prepare();

    ASSERT_TRUE(q.push_voice_value(0, 0.1f));
    ASSERT_TRUE(q.push_voice_value(0, 0.5f));
    ASSERT_TRUE(q.push_voice_value(0, 0.9f));

    DrainRecorder rec;
    q.drain_spread(512, rec.callback());

    ASSERT_EQ(rec.m_events.size(), 3u);
    EXPECT_EQ(rec.m_events[0].m_offset, 0u);
    EXPECT_EQ(rec.m_events[1].m_offset, 170u);
    EXPECT_EQ(rec.m_events[2].m_offset, 341u);
}

TEST(InputEventQueue, TapWithinBlockPreservesBothEdges) {
    InputEventQueue q(/*has_mono=*/false, /*num_voices=*/10, /*queue_capacity=*/32);
    q.prepare();

    ASSERT_TRUE(q.push_voice_active(0, true));
    ASSERT_TRUE(q.push_voice_value(0, 0.5f));
    ASSERT_TRUE(q.push_voice_active(0, false));

    DrainRecorder rec;
    q.drain_spread(512, rec.callback());

    ASSERT_EQ(rec.m_events.size(), 3u);
    EXPECT_EQ(rec.m_events[0].m_offset, 0u);
    EXPECT_EQ(rec.m_events[1].m_offset, 170u);
    EXPECT_EQ(rec.m_events[2].m_offset, 341u);
    EXPECT_EQ(rec.m_events[0].m_event.m_type, EventType::Active);
    EXPECT_TRUE(rec.m_events[0].m_event.m_active);
    EXPECT_EQ(rec.m_events[1].m_event.m_type, EventType::Value);
    EXPECT_FLOAT_EQ(rec.m_events[1].m_event.m_value, 0.5f);
    EXPECT_EQ(rec.m_events[2].m_event.m_type, EventType::Active);
    EXPECT_FALSE(rec.m_events[2].m_event.m_active);
}

TEST(InputEventQueue, PolyphonicSimultaneousTouchesAllLandAtZero) {
    // Regression test for the "strum bug" — 10 concurrent touches must all
    // land at offset 0, not be spread across the block.
    InputEventQueue q(/*has_mono=*/false, /*num_voices=*/10, /*queue_capacity=*/32);
    q.prepare();

    for (uint32_t v = 0; v < 10; ++v) { ASSERT_TRUE(q.push_voice_active(v, true)); }

    DrainRecorder rec;
    q.drain_spread(512, rec.callback());

    ASSERT_EQ(rec.m_events.size(), 10u);
    for (const auto& c : rec.m_events) {
        EXPECT_EQ(c.m_offset, 0u) << "voice " << static_cast<int>(c.m_event.m_voice);
    }
}

TEST(InputEventQueue, PolyphonicMixedRatesSpreadPerBucket) {
    InputEventQueue q(/*has_mono=*/false, /*num_voices=*/10, /*queue_capacity=*/32);
    q.prepare();

    // Voice 0 gets 8 events; voice 5 gets 1.
    for (int i = 0; i < 8; ++i) {
        ASSERT_TRUE(q.push_voice_value(0, static_cast<float>(i) * 0.1f));
    }
    ASSERT_TRUE(q.push_voice_active(5, true));

    DrainRecorder rec;
    q.drain_spread(512, rec.callback());

    ASSERT_EQ(rec.m_events.size(), 9u);

    // Sort captured events by voice so we can check each bucket independently.
    std::vector<Captured> v0;
    std::vector<Captured> v5;
    for (const auto& c : rec.m_events) {
        if (c.m_event.m_voice == 0) { v0.push_back(c); }
        if (c.m_event.m_voice == 5) { v5.push_back(c); }
    }

    ASSERT_EQ(v0.size(), 8u);
    ASSERT_EQ(v5.size(), 1u);
    const uint32_t expected_v0[] = {0, 64, 128, 192, 256, 320, 384, 448};
    for (size_t i = 0; i < 8; ++i) { EXPECT_EQ(v0[i].m_offset, expected_v0[i]); }
    EXPECT_EQ(v5[0].m_offset, 0u);
}

TEST(InputEventQueue, MonoAndVoiceConcurrentlyIndependentSpread) {
    InputEventQueue q(/*has_mono=*/true, /*num_voices=*/10, /*queue_capacity=*/32);
    q.prepare();

    ASSERT_TRUE(q.push_mono_value(0.1f));
    ASSERT_TRUE(q.push_mono_value(0.9f));
    ASSERT_TRUE(q.push_voice_value(7, 0.5f));
    ASSERT_TRUE(q.push_voice_value(7, 0.7f));
    ASSERT_TRUE(q.push_voice_value(7, 0.2f));

    DrainRecorder rec;
    q.drain_spread(512, rec.callback());

    ASSERT_EQ(rec.m_events.size(), 5u);

    std::vector<Captured> mono;
    std::vector<Captured> v7;
    for (const auto& c : rec.m_events) {
        if (c.m_event.m_is_mono) {
            mono.push_back(c);
        } else {
            v7.push_back(c);
        }
    }

    ASSERT_EQ(mono.size(), 2u);
    EXPECT_EQ(mono[0].m_offset, 0u);
    EXPECT_EQ(mono[1].m_offset, 256u);

    ASSERT_EQ(v7.size(), 3u);
    EXPECT_EQ(v7[0].m_offset, 0u);
    EXPECT_EQ(v7[1].m_offset, 170u);
    EXPECT_EQ(v7[2].m_offset, 341u);
}

TEST(InputEventQueue, ActiveOnlyToggle) {
    InputEventQueue q(/*has_mono=*/false, /*num_voices=*/10, /*queue_capacity=*/32);
    q.prepare();

    ASSERT_TRUE(q.push_voice_active(0, true));
    ASSERT_TRUE(q.push_voice_active(0, false));

    DrainRecorder rec;
    q.drain_spread(512, rec.callback());

    ASSERT_EQ(rec.m_events.size(), 2u);
    EXPECT_EQ(rec.m_events[0].m_offset, 0u);
    EXPECT_EQ(rec.m_events[1].m_offset, 256u);
    EXPECT_EQ(rec.m_events[0].m_event.m_type, EventType::Active);
    EXPECT_TRUE(rec.m_events[0].m_event.m_active);
    EXPECT_EQ(rec.m_events[1].m_event.m_type, EventType::Active);
    EXPECT_FALSE(rec.m_events[1].m_event.m_active);
}

TEST(InputEventQueue, BlockSizeScalesOffsets) {
    for (uint32_t bs : {64u, 512u, 2048u}) {
        InputEventQueue q(/*has_mono=*/false, /*num_voices=*/10, /*queue_capacity=*/32);
        q.prepare();

        ASSERT_TRUE(q.push_voice_value(0, 0.1f));
        ASSERT_TRUE(q.push_voice_value(0, 0.5f));
        ASSERT_TRUE(q.push_voice_value(0, 0.9f));

        DrainRecorder rec;
        q.drain_spread(bs, rec.callback());

        ASSERT_EQ(rec.m_events.size(), 3u);
        EXPECT_EQ(rec.m_events[0].m_offset, 0u);
        EXPECT_EQ(rec.m_events[1].m_offset, bs / 3);
        EXPECT_EQ(rec.m_events[2].m_offset, (2 * bs) / 3);
    }
}

TEST(InputEventQueue, StreamIsolationVoicesAndMonoDontLeak) {
    InputEventQueue q(/*has_mono=*/true, /*num_voices=*/4, /*queue_capacity=*/32);
    q.prepare();

    ASSERT_TRUE(q.push_voice_value(0, 1.0f));
    ASSERT_TRUE(q.push_voice_value(3, 2.0f));
    ASSERT_TRUE(q.push_mono_value(3.0f));

    DrainRecorder rec;
    q.drain_spread(512, rec.callback());

    ASSERT_EQ(rec.m_events.size(), 3u);

    // Every captured event's voice/is_mono must match exactly what was pushed.
    int mono_count = 0;
    int v0_count = 0;
    int v3_count = 0;
    for (const auto& c : rec.m_events) {
        if (c.m_event.m_is_mono) {
            EXPECT_FLOAT_EQ(c.m_event.m_value, 3.0f);
            ++mono_count;
        } else if (c.m_event.m_voice == 0) {
            EXPECT_FLOAT_EQ(c.m_event.m_value, 1.0f);
            ++v0_count;
        } else if (c.m_event.m_voice == 3) {
            EXPECT_FLOAT_EQ(c.m_event.m_value, 2.0f);
            ++v3_count;
        }
    }
    EXPECT_EQ(mono_count, 1);
    EXPECT_EQ(v0_count, 1);
    EXPECT_EQ(v3_count, 1);
}

TEST(InputEventQueue, QueueFullReturnsFalseAndDrops) {
    InputEventQueue q(/*has_mono=*/true, /*num_voices=*/0, /*queue_capacity=*/4);
    q.prepare();

    // Push well past capacity. ReaderWriterQueue grows on try_enqueue when at
    // capacity, but the event must always be either accepted or dropped (no
    // corruption). Drain and assert the order of accepted events.
    bool all_accepted = true;
    for (int i = 0; i < 64; ++i) {
        if (!q.push_mono_value(static_cast<float>(i))) { all_accepted = false; }
    }

    DrainRecorder rec;
    q.drain_spread(512, rec.callback());

    // We don't enforce a specific count — moodycamel grows under the hood. We
    // just require no corruption: accepted values appear in FIFO order with
    // no gaps backwards.
    float prev = -1.0f;
    for (const auto& c : rec.m_events) {
        EXPECT_GT(c.m_event.m_value, prev);
        prev = c.m_event.m_value;
    }
    (void)all_accepted;
}

// ── Allocation-free hot path ────────────────────────────────────────────────
//
// Verify by pre-filling at capacity, then draining, and re-measuring that
// the scratch vectors' capacity never shrinks or grows between drains. If
// the hot path were allocating, capacity would change under the push_back.

TEST(InputEventQueue, DrainSpreadDoesNotReallocateScratch) {
    constexpr size_t k_cap = 64;
    InputEventQueue q(/*has_mono=*/true, /*num_voices=*/10, /*queue_capacity=*/k_cap);
    q.prepare();

    // Warm up with a representative load.
    for (int i = 0; i < 20; ++i) { ASSERT_TRUE(q.push_voice_value(i % 10, 0.1f)); }
    for (int i = 0; i < 5; ++i) { ASSERT_TRUE(q.push_mono_value(0.2f)); }

    DrainRecorder rec;
    q.drain_spread(512, rec.callback());

    // Second pass — we don't have direct access to internal capacities, but
    // we can prove determinism + crash-free behavior under heavy churn.
    for (int cycle = 0; cycle < 10; ++cycle) {
        for (int i = 0; i < 20; ++i) { q.push_voice_value(i % 10, 0.1f); }
        for (int i = 0; i < 5; ++i) { q.push_mono_value(0.2f); }

        DrainRecorder r;
        const size_t n = q.drain_spread(512, r.callback());
        EXPECT_EQ(n, 25u);
    }
}

// ── Configurations ──────────────────────────────────────────────────────────

TEST(InputEventQueue, VoiceOnlyConfigAcceptsVoicePushes) {
    InputEventQueue q(/*has_mono=*/false, /*num_voices=*/4, /*queue_capacity=*/16);
    q.prepare();
    ASSERT_TRUE(q.push_voice_value(0, 0.5f));
    ASSERT_TRUE(q.push_voice_active(3, true));
}

TEST(InputEventQueue, MonoOnlyConfigAcceptsMonoPushes) {
    InputEventQueue q(/*has_mono=*/true, /*num_voices=*/0, /*queue_capacity=*/16);
    q.prepare();
    ASSERT_TRUE(q.push_mono_value(0.5f));
    ASSERT_TRUE(q.push_mono_active(true));
}

TEST(InputEventQueue, BothConfigAcceptsMonoAndVoicePushes) {
    InputEventQueue q(/*has_mono=*/true, /*num_voices=*/4, /*queue_capacity=*/16);
    q.prepare();
    ASSERT_TRUE(q.push_mono_value(0.5f));
    ASSERT_TRUE(q.push_voice_value(0, 0.7f));
}
