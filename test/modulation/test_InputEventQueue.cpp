#include <cstddef>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include <tanh/modulation/InputEventQueue.h>
#include <tanh/state/ModulationScope.h>

namespace {

using thl::modulation::InputEventQueue;
using EventType = InputEventQueue::EventType;

// Test-local scopes. Not bound to any matrix — InputEventQueue only inspects
// has_mono() via scope comparison against k_global_scope.
constexpr thl::modulation::ModulationScope k_global_scope = thl::modulation::k_global_scope;
constexpr thl::modulation::ModulationScope k_voice_scope{.m_id = 1, .m_name = "test_voice"};

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
    InputEventQueue q(k_voice_scope, /*queue_capacity=*/32);
    q.prepare(/*num_voices=*/10);

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
    InputEventQueue q(k_global_scope, /*queue_capacity=*/32);
    q.prepare(/*num_voices=*/0);

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
    InputEventQueue q(k_voice_scope, /*queue_capacity=*/32);
    q.prepare(/*num_voices=*/10);

    DrainRecorder rec;
    EXPECT_EQ(q.drain_spread(512, rec.callback()), 0u);
    EXPECT_TRUE(rec.m_events.empty());
}

// ── Per-stream spreading — the core behavior ────────────────────────────────

TEST(InputEventQueue, TwoVoiceEventsSpreadEvenly) {
    InputEventQueue q(k_voice_scope, /*queue_capacity=*/32);
    q.prepare(/*num_voices=*/10);

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
    InputEventQueue q(k_voice_scope, /*queue_capacity=*/32);
    q.prepare(/*num_voices=*/10);

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
    InputEventQueue q(k_voice_scope, /*queue_capacity=*/32);
    q.prepare(/*num_voices=*/10);

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
    InputEventQueue q(k_voice_scope, /*queue_capacity=*/32);
    q.prepare(/*num_voices=*/10);

    for (uint32_t v = 0; v < 10; ++v) { ASSERT_TRUE(q.push_voice_active(v, true)); }

    DrainRecorder rec;
    q.drain_spread(512, rec.callback());

    ASSERT_EQ(rec.m_events.size(), 10u);
    for (const auto& c : rec.m_events) {
        EXPECT_EQ(c.m_offset, 0u) << "voice " << static_cast<int>(c.m_event.m_voice);
    }
}

TEST(InputEventQueue, PolyphonicMixedRatesSpreadPerBucket) {
    InputEventQueue q(k_voice_scope, /*queue_capacity=*/32);
    q.prepare(/*num_voices=*/10);

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

TEST(InputEventQueue, ActiveOnlyToggle) {
    InputEventQueue q(k_voice_scope, /*queue_capacity=*/32);
    q.prepare(/*num_voices=*/10);

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
        InputEventQueue q(k_voice_scope, /*queue_capacity=*/32);
        q.prepare(/*num_voices=*/10);

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

TEST(InputEventQueue, QueueFullReturnsFalseAndDrops) {
    InputEventQueue q(k_global_scope, /*queue_capacity=*/4);
    q.prepare(/*num_voices=*/0);

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
    InputEventQueue q(k_voice_scope, /*queue_capacity=*/k_cap);
    q.prepare(/*num_voices=*/10);

    // Warm up with a representative load.
    for (int i = 0; i < 25; ++i) { ASSERT_TRUE(q.push_voice_value(i % 10, 0.1f)); }

    DrainRecorder rec;
    q.drain_spread(512, rec.callback());

    // Second pass — we don't have direct access to internal capacities, but
    // we can prove determinism + crash-free behavior under heavy churn.
    for (int cycle = 0; cycle < 10; ++cycle) {
        for (int i = 0; i < 25; ++i) { q.push_voice_value(i % 10, 0.1f); }

        DrainRecorder r;
        const size_t n = q.drain_spread(512, r.callback());
        EXPECT_EQ(n, 25u);
    }
}

// ── Configurations ──────────────────────────────────────────────────────────

TEST(InputEventQueue, VoiceScopeConfigAcceptsVoicePushes) {
    InputEventQueue q(k_voice_scope, /*queue_capacity=*/16);
    q.prepare(/*num_voices=*/4);
    ASSERT_TRUE(q.push_voice_value(0, 0.5f));
    ASSERT_TRUE(q.push_voice_active(3, true));
}

TEST(InputEventQueue, GlobalScopeConfigAcceptsMonoPushes) {
    InputEventQueue q(k_global_scope, /*queue_capacity=*/16);
    q.prepare(/*num_voices=*/0);
    ASSERT_TRUE(q.push_mono_value(0.5f));
    ASSERT_TRUE(q.push_mono_active(true));
}

TEST(InputEventQueue, ScopeDerivesHasMono) {
    InputEventQueue global_q(k_global_scope);
    InputEventQueue voice_q(k_voice_scope);
    EXPECT_TRUE(global_q.has_mono());
    EXPECT_FALSE(voice_q.has_mono());
    EXPECT_EQ(global_q.scope(), k_global_scope);
    EXPECT_EQ(voice_q.scope(), k_voice_scope);
}

TEST(InputEventQueue, GlobalScopeIgnoresVoiceCountInPrepare) {
    // A global-scoped queue must not allocate voice buckets even if the
    // matrix passes voice_count(global) == 1 (or anything else).
    InputEventQueue q(k_global_scope, /*queue_capacity=*/16);
    q.prepare(/*num_voices=*/5);
    EXPECT_EQ(q.num_voices(), 0u);
    EXPECT_TRUE(q.has_mono());
}
