#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "TestHelpers.h"

#include <tanh/modulation/ModulationMatrix.h>
#include <tanh/modulation/SmartHandle.h>
#include <tanh/state/State.h>

using namespace thl::modulation;

// =============================================================================
// Partially active source — marks only configurable samples as active
// =============================================================================

class PartiallyActiveSource : public ModulationSource {
public:
    float m_value = 0.7f;
    std::vector<bool> m_active_pattern;

    PartiallyActiveSource() : ModulationSource(true, 0, false) {}

    void prepare(double /*sample_rate*/, size_t samples_per_block) override {
        resize_buffers(samples_per_block);
        if (m_active_pattern.empty()) { m_active_pattern.assign(samples_per_block, true); }
    }

    void process(size_t num_samples, size_t offset = 0) override {
        for (size_t i = offset; i < offset + num_samples; ++i) {
            m_output_buffer[i] = m_value;
            if (m_active_pattern[i]) { set_output_active(static_cast<uint32_t>(i)); }
        }
        if (num_samples > 0) { record_change_point(static_cast<uint32_t>(offset)); }
    }
};

class PartiallyActivePolySource : public ModulationSource {
public:
    std::vector<float> m_voice_values;
    std::vector<std::vector<bool>> m_voice_active_patterns;

    explicit PartiallyActivePolySource(uint32_t num_voices)
        : ModulationSource(false, num_voices, false) {}

    void prepare(double /*sample_rate*/, size_t samples_per_block) override {
        resize_buffers(samples_per_block);
        if (m_voice_active_patterns.empty()) {
            m_voice_active_patterns.resize(num_voices(),
                                           std::vector<bool>(samples_per_block, true));
        }
    }

    void process_voice(uint32_t voice_index, size_t num_samples, size_t offset = 0) override {
        float* out = voice_output(voice_index);
        const float val = m_voice_values[voice_index];
        for (size_t i = offset; i < offset + num_samples; ++i) {
            out[i] = val;
            if (m_voice_active_patterns[voice_index][i]) {
                set_voice_output_active(voice_index, static_cast<uint32_t>(i));
            }
        }
        if (num_samples > 0) {
            record_voice_change_point(voice_index, static_cast<uint32_t>(offset));
        }
    }
};

// =============================================================================
// Sparse buffer allocation tests
// =============================================================================

TEST(PolyphonicModulation, PolyAdditive_AllocatesAdditiveVoiceBuffersOnly) {
    thl::State state;
    state.create("freq", modulatable_float(0.5f));
    ModulationMatrix matrix(state);

    PolyTestSource poly(2);
    poly.m_voice_values = {0.1f, 0.2f};
    matrix.add_source("poly", &poly);
    matrix.get_smart_handle<float>("freq");
    matrix.add_routing({"poly", "freq", 1.0f});
    matrix.prepare(k_sample_rate, k_block_size);

    const auto* target = matrix.get_target("freq");
    ASSERT_NE(target, nullptr);
    ASSERT_NE(voice_of(target), nullptr);
    EXPECT_TRUE(voice_of(target)->m_has_additive);
    EXPECT_FALSE(voice_of(target)->m_has_replace);
    EXPECT_EQ(voice_of(target)->m_num_voices, 2u);
}

TEST(PolyphonicModulation, PolyReplace_AllocatesReplaceVoiceBuffersOnly) {
    thl::State state;
    state.create("freq", modulatable_float(0.5f));
    ModulationMatrix matrix(state);

    PolyTestSource poly(2);
    poly.m_voice_values = {0.3f, 0.7f};
    matrix.add_source("poly", &poly);
    matrix.get_smart_handle<float>("freq");
    matrix.add_routing({"poly", "freq", 1.0f, 0, DepthMode::Normalized, CombineMode::Replace});
    matrix.prepare(k_sample_rate, k_block_size);

    const auto* target = matrix.get_target("freq");
    ASSERT_NE(target, nullptr);
    ASSERT_NE(voice_of(target), nullptr);
    EXPECT_FALSE(voice_of(target)->m_has_additive);
    EXPECT_TRUE(voice_of(target)->m_has_replace);
}

TEST(PolyphonicModulation, PolyBoth_AllocatesBothVoiceBuffers) {
    thl::State state;
    state.create("freq", modulatable_float(0.5f));
    ModulationMatrix matrix(state);

    PolyTestSource poly_add(2);
    poly_add.m_voice_values = {0.1f, 0.2f};
    PolyTestSource poly_replace(2);
    poly_replace.m_voice_values = {0.3f, 0.4f};

    matrix.add_source("add", &poly_add);
    matrix.add_source("rep", &poly_replace);
    matrix.get_smart_handle<float>("freq");
    matrix.add_routing({"add", "freq", 1.0f});
    matrix.add_routing({"rep", "freq", 1.0f, 0, DepthMode::Normalized, CombineMode::Replace});
    matrix.prepare(k_sample_rate, k_block_size);

    const auto* target = matrix.get_target("freq");
    ASSERT_NE(target, nullptr);
    ASSERT_NE(voice_of(target), nullptr);
    EXPECT_TRUE(voice_of(target)->m_has_additive);
    EXPECT_TRUE(voice_of(target)->m_has_replace);
}

TEST(PolyphonicModulation, MonoOnly_NoVoiceBuffers) {
    thl::State state;
    state.create("freq", modulatable_float(0.5f));
    ModulationMatrix matrix(state);

    TestLFOSource lfo;
    lfo.m_frequency = 1.0f;
    matrix.add_source("lfo", &lfo);
    matrix.get_smart_handle<float>("freq");
    matrix.add_routing({"lfo", "freq", 1.0f});
    matrix.prepare(k_sample_rate, k_block_size);

    const auto* target = matrix.get_target("freq");
    ASSERT_NE(target, nullptr);
    EXPECT_EQ(voice_of(target), nullptr);
    EXPECT_TRUE(has_mono_additive(target));
}

TEST(PolyphonicModulation, VoiceBuffers_FreedWhenPolyRoutingRemoved) {
    thl::State state;
    state.create("freq", modulatable_float(0.5f));
    ModulationMatrix matrix(state);

    PolyTestSource poly(2);
    poly.m_voice_values = {0.1f, 0.2f};
    matrix.add_source("poly", &poly);
    matrix.get_smart_handle<float>("freq");
    matrix.add_routing({"poly", "freq", 1.0f});
    matrix.prepare(k_sample_rate, k_block_size);

    ASSERT_NE(voice_of(matrix.get_target("freq")), nullptr);

    matrix.remove_routing("poly", "freq");
    const auto* target = matrix.get_target("freq");
    EXPECT_EQ(voice_of(target), nullptr);
}

// =============================================================================
// RoutingMode determination tests
// =============================================================================

TEST(PolyphonicModulation, PolySource_PolyTarget_IsPolyToPoly) {
    thl::State state;
    state.create("freq", modulatable_float(0.5f));
    ModulationMatrix matrix(state);

    PolyTestSource poly(2);
    poly.m_voice_values = {0.1f, 0.2f};
    matrix.add_source("poly", &poly);
    matrix.get_smart_handle<float>("freq");
    matrix.add_routing({"poly", "freq", 1.0f});
    matrix.prepare(k_sample_rate, k_block_size);

    // Target should have voice buffers (poly target)
    ASSERT_NE(voice_of(matrix.get_target("freq")), nullptr);
}

TEST(PolyphonicModulation, MonoSource_PolyTarget_IsMonoToPoly) {
    thl::State state;
    state.create("freq", modulatable_float(0.5f));
    ModulationMatrix matrix(state);

    // Add poly source to make the target polyphonic
    PolyTestSource poly(2);
    poly.m_voice_values = {0.0f, 0.0f};
    matrix.add_source("poly", &poly);
    matrix.get_smart_handle<float>("freq");
    matrix.add_routing({"poly", "freq", 0.0f});

    // Add mono source to the same (now poly) target
    TestLFOSource lfo;
    lfo.m_frequency = 1.0f;
    matrix.add_source("lfo", &lfo);
    matrix.add_routing({"lfo", "freq", 1.0f});
    matrix.prepare(k_sample_rate, k_block_size);

    // Target should be poly (voice buffers allocated)
    const auto* target = matrix.get_target("freq");
    ASSERT_NE(voice_of(target), nullptr);
}

TEST(PolyphonicModulation, MonoSource_MonoTarget_IsMonoToMono) {
    thl::State state;
    state.create("freq", modulatable_float(0.5f));
    ModulationMatrix matrix(state);

    TestLFOSource lfo;
    lfo.m_frequency = 1.0f;
    matrix.add_source("lfo", &lfo);
    matrix.get_smart_handle<float>("freq");
    matrix.add_routing({"lfo", "freq", 1.0f});
    matrix.prepare(k_sample_rate, k_block_size);

    const auto* target = matrix.get_target("freq");
    EXPECT_EQ(voice_of(target), nullptr);
    EXPECT_TRUE(has_mono_additive(target));
}

// =============================================================================
// Additive processing correctness
// =============================================================================

TEST(PolyphonicModulation, PolyToPoly_VoiceBuffersFilled) {
    thl::State state;
    state.create("freq", modulatable_float(0.5f));
    ModulationMatrix matrix(state);

    PolyTestSource poly(2);
    poly.m_voice_values = {0.3f, 0.7f};
    matrix.add_source("poly", &poly);
    matrix.get_smart_handle<float>("freq");
    matrix.add_routing({"poly", "freq", 1.0f, 0, DepthMode::Absolute});
    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    const auto* target = matrix.get_target("freq");
    ASSERT_NE(voice_of(target), nullptr);
    EXPECT_FLOAT_EQ(voice_of(target)->additive_voice(0)[0], 0.3f);
    EXPECT_FLOAT_EQ(voice_of(target)->additive_voice(1)[0], 0.7f);
}

TEST(PolyphonicModulation, MonoToPoly_BroadcastsToAllVoices) {
    thl::State state;
    state.create("freq", modulatable_float(0.5f));
    ModulationMatrix matrix(state);

    // Poly source to make target polyphonic
    PolyTestSource poly(2);
    poly.m_voice_values = {0.0f, 0.0f};
    matrix.add_source("poly", &poly);
    matrix.get_smart_handle<float>("freq");
    matrix.add_routing({"poly", "freq", 0.0f});

    // Mono source broadcasting to all voices
    TestLFOSource lfo;
    lfo.m_frequency = 0.0f;  // DC
    lfo.m_waveform = LFOWaveform::Sine;
    matrix.add_source("lfo", &lfo);
    matrix.add_routing({"lfo", "freq", 1.0f, 0, DepthMode::Absolute});
    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    const auto* target = matrix.get_target("freq");
    ASSERT_NE(voice_of(target), nullptr);
    ASSERT_TRUE(voice_of(target)->m_has_additive);

    // Both voices should have the same mono value broadcast
    const float v0 = voice_of(target)->additive_voice(0)[0];
    const float v1 = voice_of(target)->additive_voice(1)[0];
    EXPECT_FLOAT_EQ(v0, v1);
}

TEST(PolyphonicModulation, CombinedSource_MakesTargetPoly_UsesVoicePath) {
    thl::State state;
    state.create("freq", modulatable_float(0.5f));
    ModulationMatrix matrix(state);

    // A combined source has voice output, so it makes the target poly.
    // The matrix uses the voice path (PolyToPoly) for combined → poly target.
    CombinedTestSource combined(2);
    combined.m_mono_value = 0.5f;
    combined.m_voice_values = {0.1f, 0.2f};
    matrix.add_source("combined", &combined);
    matrix.get_smart_handle<float>("freq");
    matrix.add_routing({"combined", "freq", 1.0f, 0, DepthMode::Absolute});
    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    const auto* target = matrix.get_target("freq");
    ASSERT_NE(voice_of(target), nullptr);
    EXPECT_TRUE(voice_of(target)->m_has_additive);
    EXPECT_FLOAT_EQ(voice_of(target)->additive_voice(0)[0], 0.1f);
    EXPECT_FLOAT_EQ(voice_of(target)->additive_voice(1)[0], 0.2f);
}

// =============================================================================
// Replace processing correctness
// =============================================================================

TEST(PolyphonicModulation, Replace_MonoToMono_OverridesBase) {
    thl::State state;
    state.create("freq", modulatable_float(0.5f));
    ModulationMatrix matrix(state);

    TestLFOSource lfo;
    lfo.m_frequency = 0.0f;  // DC
    matrix.add_source("lfo", &lfo);
    auto handle = matrix.get_smart_handle<float>("freq");
    matrix.add_routing({"lfo", "freq", 1.0f, 0, DepthMode::Absolute, CombineMode::Replace});
    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    const auto* target = matrix.get_target("freq");
    ASSERT_TRUE(has_mono_replace(target));
    EXPECT_EQ(mono_of(target)->m_replace_active[0], 1);

    // SmartHandle should read replace value, not the atomic base
    const float loaded = handle.load(0);
    EXPECT_NE(loaded, 0.5f);  // not the base value
}

TEST(PolyphonicModulation, Replace_PolyToPoly_PerVoiceOverride) {
    thl::State state;
    state.create("freq", modulatable_float(0.5f));
    ModulationMatrix matrix(state);

    PolyTestSource poly(2);
    poly.m_voice_values = {0.3f, 0.8f};
    matrix.add_source("poly", &poly);
    auto handle = matrix.get_smart_handle<float>("freq");
    matrix.add_routing({"poly", "freq", 1.0f, 0, DepthMode::Absolute, CombineMode::Replace});
    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    const auto* target = matrix.get_target("freq");
    ASSERT_NE(voice_of(target), nullptr);
    ASSERT_TRUE(voice_of(target)->m_has_replace);

    // Each voice should have its own replace value
    EXPECT_FLOAT_EQ(voice_of(target)->replace_voice(0)[0], 0.3f);
    EXPECT_FLOAT_EQ(voice_of(target)->replace_voice(1)[0], 0.8f);
    EXPECT_EQ(voice_of(target)->replace_active_voice(0)[0], 1);
    EXPECT_EQ(voice_of(target)->replace_active_voice(1)[0], 1);

    // SmartHandle voice 0 and voice 1 should differ
    EXPECT_FLOAT_EQ(handle.load(0, 0), 0.3f);
    EXPECT_FLOAT_EQ(handle.load(0, 1), 0.8f);
}

TEST(PolyphonicModulation, Replace_PlusAdditive_Composes) {
    thl::State state;
    state.create("freq", modulatable_float(0.5f));
    ModulationMatrix matrix(state);

    PolyTestSource poly_replace(2);
    poly_replace.m_voice_values = {0.4f, 0.6f};
    PolyTestSource poly_add(2);
    poly_add.m_voice_values = {0.1f, 0.1f};

    matrix.add_source("rep", &poly_replace);
    matrix.add_source("add", &poly_add);
    auto handle = matrix.get_smart_handle<float>("freq");
    matrix.add_routing({"rep", "freq", 1.0f, 0, DepthMode::Absolute, CombineMode::Replace});
    matrix.add_routing({"add", "freq", 1.0f, 0, DepthMode::Absolute, CombineMode::Additive});
    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    // final = replace + additive
    EXPECT_NEAR(handle.load(0, 0), 0.5f, 0.01f);  // 0.4 + 0.1
    EXPECT_NEAR(handle.load(0, 1), 0.7f, 0.01f);  // 0.6 + 0.1
}

TEST(PolyphonicModulation, Replace_Inactive_FallsBackToBase) {
    thl::State state;
    state.create("freq", modulatable_float(0.5f));
    ModulationMatrix matrix(state);

    // No replace source active — just additive mono
    TestLFOSource lfo;
    lfo.m_frequency = 0.0f;
    matrix.add_source("lfo", &lfo);
    auto handle = matrix.get_smart_handle<float>("freq");
    matrix.add_routing({"lfo", "freq", 0.0f, 0, DepthMode::Absolute});
    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    // No replace active, should fall back to base (0.5)
    EXPECT_FLOAT_EQ(handle.load(0), 0.5f);
}

TEST(PolyphonicModulation, Replace_EqualPriority_AddOrderWins) {
    thl::State state;
    state.create("freq", modulatable_float(0.5f));
    ModulationMatrix matrix(state);

    // Two Replace routings with equal (default) priority on the same target
    // compose in add-order — the later-added routing writes last per sample.
    PolyTestSource poly_a(2);
    poly_a.m_voice_values = {0.9f, 0.9f};
    PolyTestSource poly_b(2);
    poly_b.m_voice_values = {0.1f, 0.1f};
    matrix.add_source("poly_a", &poly_a);
    matrix.add_source("poly_b", &poly_b);
    auto handle = matrix.get_smart_handle<float>("freq");
    EXPECT_NE(
        matrix.add_routing({"poly_a", "freq", 1.0f, 0, DepthMode::Absolute, CombineMode::Replace}),
        k_invalid_routing_id);
    EXPECT_NE(
        matrix.add_routing({"poly_b", "freq", 1.0f, 0, DepthMode::Absolute, CombineMode::Replace}),
        k_invalid_routing_id);
    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    // Equal priority: poly_b added second, applied last in the deferred
    // post-pass, wins the overlapping active region.
    EXPECT_FLOAT_EQ(handle.load(0, 0), 0.1f);
    EXPECT_FLOAT_EQ(handle.load(0, 1), 0.1f);
}

TEST(PolyphonicModulation, Replace_HigherPriorityWins) {
    thl::State state;
    state.create("freq", modulatable_float(0.5f));
    ModulationMatrix matrix(state);

    // Higher priority wins even when added earlier.
    PolyTestSource poly_hi(2);
    poly_hi.m_voice_values = {0.9f, 0.9f};
    PolyTestSource poly_lo(2);
    poly_lo.m_voice_values = {0.1f, 0.1f};
    matrix.add_source("poly_hi", &poly_hi);
    matrix.add_source("poly_lo", &poly_lo);
    auto handle = matrix.get_smart_handle<float>("freq");

    ModulationRouting hi{"poly_hi", "freq", 1.0f, 0, DepthMode::Absolute, CombineMode::Replace};
    hi.m_priority = 10;
    EXPECT_NE(matrix.add_routing(hi), k_invalid_routing_id);

    ModulationRouting lo{"poly_lo", "freq", 1.0f, 0, DepthMode::Absolute, CombineMode::Replace};
    lo.m_priority = 1;
    EXPECT_NE(matrix.add_routing(lo), k_invalid_routing_id);

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    // hi (priority 10) is sorted after lo (priority 1) in ascending order,
    // so hi applies last and its value wins despite being added first.
    EXPECT_FLOAT_EQ(handle.load(0, 0), 0.9f);
    EXPECT_FLOAT_EQ(handle.load(0, 1), 0.9f);
}

TEST(PolyphonicModulation, Replace_HigherPriorityInactive_LowerPriorityWins) {
    thl::State state;
    state.create("freq", modulatable_float(0.5f));
    ModulationMatrix matrix(state);

    // Higher-priority Replace is inactive → lower-priority Replace's write survives.
    PartiallyActivePolySource poly_hi(2);
    poly_hi.m_voice_values = {0.9f, 0.9f};
    poly_hi.m_voice_active_patterns = {std::vector<bool>(k_block_size, false),
                                       std::vector<bool>(k_block_size, false)};

    PolyTestSource poly_lo(2);
    poly_lo.m_voice_values = {0.1f, 0.1f};

    matrix.add_source("poly_hi", &poly_hi);
    matrix.add_source("poly_lo", &poly_lo);
    auto handle = matrix.get_smart_handle<float>("freq");

    ModulationRouting hi{"poly_hi", "freq", 1.0f, 0, DepthMode::Absolute, CombineMode::Replace};
    hi.m_priority = 10;
    matrix.add_routing(hi);

    ModulationRouting lo{"poly_lo", "freq", 1.0f, 0, DepthMode::Absolute, CombineMode::Replace};
    lo.m_priority = 1;
    matrix.add_routing(lo);

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    // hi is inactive over the whole block, so its Replace contributes nothing.
    // lo's write (active) survives in both voices.
    EXPECT_FLOAT_EQ(handle.load(0, 0), 0.1f);
    EXPECT_FLOAT_EQ(handle.load(0, 1), 0.1f);
}

TEST(PolyphonicModulation, Replace_SingleRouting_RegressionIdenticalToLegacy) {
    thl::State state;
    state.create("freq", modulatable_float(0.5f));
    ModulationMatrix matrix(state);

    // Regression: a single Replace routing (with priority set) should behave
    // identically to the legacy single-Replace fast path — no multi-Replace
    // composition, no deferral.
    PolyTestSource poly_a(2);
    poly_a.m_voice_values = {0.42f, 0.77f};
    matrix.add_source("poly_a", &poly_a);
    auto handle = matrix.get_smart_handle<float>("freq");

    ModulationRouting r{"poly_a", "freq", 1.0f, 0, DepthMode::Absolute, CombineMode::Replace};
    r.m_priority = 7;
    matrix.add_routing(r);

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    EXPECT_FLOAT_EQ(handle.load(0, 0), 0.42f);
    EXPECT_FLOAT_EQ(handle.load(0, 1), 0.77f);
}

// =============================================================================
// SmartHandle voice_index tests
// =============================================================================

TEST(PolyphonicModulation, Load_WithVoiceIndex) {
    thl::State state;
    state.create("freq", modulatable_float(0.5f));
    ModulationMatrix matrix(state);

    PolyTestSource poly(2);
    poly.m_voice_values = {0.2f, 0.8f};
    matrix.add_source("poly", &poly);
    auto handle = matrix.get_smart_handle<float>("freq");
    matrix.add_routing({"poly", "freq", 1.0f, 0, DepthMode::Absolute});
    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    // Voice 0 and voice 1 should have different additive values
    const float v0 = handle.load(0, 0);
    const float v1 = handle.load(0, 1);
    EXPECT_NE(v0, v1);
    EXPECT_NEAR(v0, 0.5f + 0.2f, 0.01f);
    EXPECT_NEAR(v1, 0.5f + 0.8f, 0.01f);
}

TEST(PolyphonicModulation, Load_NoVoiceBuffers_IgnoresVoiceIndex) {
    thl::State state;
    state.create("freq", modulatable_float(0.5f));
    ModulationMatrix matrix(state);

    TestLFOSource lfo;
    lfo.m_frequency = 0.0f;
    matrix.add_source("lfo", &lfo);
    auto handle = matrix.get_smart_handle<float>("freq");
    matrix.add_routing({"lfo", "freq", 0.0f, 0, DepthMode::Absolute});
    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    // No voice buffers — voice_index should be ignored
    EXPECT_FLOAT_EQ(handle.load(0, 0), handle.load(0, 5));
}

// =============================================================================
// Serialization tests
// =============================================================================

TEST(PolyphonicModulation, ToJson_SerializesAllRoutingFields) {
    thl::State state;
    state.create("freq", modulatable_float(0.5f));
    ModulationMatrix matrix(state);

    TestLFOSource lfo;
    matrix.add_source("lfo1", &lfo);
    matrix.get_smart_handle<float>("freq");
    matrix.add_routing({"lfo1", "freq", 0.5f, 8, DepthMode::Normalized, CombineMode::Replace});
    matrix.prepare(k_sample_rate, k_block_size);

    auto json = matrix.to_json(false);
    ASSERT_TRUE(json.is_array());
    ASSERT_EQ(json.size(), 1u);

    const auto& r = json[0];
    EXPECT_EQ(r["source_id"], "lfo1");
    EXPECT_EQ(r["target_id"], "freq");
    EXPECT_FLOAT_EQ(r["depth"].get<float>(), 0.5f);
    EXPECT_EQ(r["depth_mode"], "normalized");
    EXPECT_EQ(r["combine_mode"], "replace");
    EXPECT_EQ(r["max_decimation"], 8u);
}

TEST(PolyphonicModulation, FromJson_RestoresRoutings) {
    thl::State state;
    state.create("freq", modulatable_float(0.5f));
    ModulationMatrix matrix(state);

    TestLFOSource lfo;
    matrix.add_source("lfo1", &lfo);
    matrix.get_smart_handle<float>("freq");
    matrix.add_routing({"lfo1", "freq", 0.75f, 0, DepthMode::Absolute, CombineMode::Additive});
    matrix.prepare(k_sample_rate, k_block_size);

    // Serialize
    auto json = matrix.to_json(false);

    // Create new matrix and restore
    ModulationMatrix matrix2(state);
    matrix2.add_source("lfo1", &lfo);
    matrix2.get_smart_handle<float>("freq");
    matrix2.prepare(k_sample_rate, k_block_size);

    matrix2.from_json(json);
    matrix2.process(k_block_size);

    // Verify routing was restored
    auto json2 = matrix2.to_json(false);
    ASSERT_EQ(json2.size(), 1u);
    EXPECT_EQ(json2[0]["source_id"], "lfo1");
    EXPECT_FLOAT_EQ(json2[0]["depth"].get<float>(), 0.75f);
    EXPECT_EQ(json2[0]["depth_mode"], "absolute");
}

TEST(PolyphonicModulation, ToJson_OmitsDefaultPriority_LegacyRoundTrip) {
    thl::State state;
    state.create("freq", modulatable_float(0.5f));
    ModulationMatrix matrix(state);

    TestLFOSource lfo;
    matrix.add_source("lfo1", &lfo);
    matrix.get_smart_handle<float>("freq");
    matrix.add_routing({"lfo1", "freq", 0.5f, 0, DepthMode::Normalized, CombineMode::Replace});
    matrix.prepare(k_sample_rate, k_block_size);

    auto json = matrix.to_json(false);
    ASSERT_EQ(json.size(), 1u);
    // Default priority (0) is omitted → legacy presets without the field
    // round-trip to the default value with no schema churn.
    EXPECT_FALSE(json[0].contains("priority"));
}

TEST(PolyphonicModulation, ToJson_EmitsExplicitPriority) {
    thl::State state;
    state.create("freq", modulatable_float(0.5f));
    ModulationMatrix matrix(state);

    TestLFOSource lfo;
    matrix.add_source("lfo1", &lfo);
    matrix.get_smart_handle<float>("freq");
    ModulationRouting r{"lfo1", "freq", 0.5f, 0, DepthMode::Normalized, CombineMode::Replace};
    r.m_priority = 42;
    matrix.add_routing(r);
    matrix.prepare(k_sample_rate, k_block_size);

    auto json = matrix.to_json(false);
    ASSERT_EQ(json.size(), 1u);
    EXPECT_EQ(json[0]["priority"], 42u);
}

TEST(PolyphonicModulation, FromJson_RestoresPriority) {
    thl::State state;
    state.create("freq", modulatable_float(0.5f));
    ModulationMatrix matrix(state);

    TestLFOSource lfo;
    matrix.add_source("lfo1", &lfo);
    matrix.get_smart_handle<float>("freq");
    ModulationRouting r{"lfo1", "freq", 0.5f, 0, DepthMode::Normalized, CombineMode::Replace};
    r.m_priority = 42;
    matrix.add_routing(r);
    matrix.prepare(k_sample_rate, k_block_size);
    auto json = matrix.to_json(false);

    ModulationMatrix matrix2(state);
    matrix2.add_source("lfo1", &lfo);
    matrix2.get_smart_handle<float>("freq");
    matrix2.prepare(k_sample_rate, k_block_size);
    matrix2.from_json(json);

    auto json2 = matrix2.to_json(false);
    ASSERT_EQ(json2.size(), 1u);
    EXPECT_EQ(json2[0]["priority"], 42u);
}

TEST(PolyphonicModulation, FromJson_MissingPriority_DefaultsToZero) {
    thl::State state;
    state.create("freq", modulatable_float(0.5f));
    ModulationMatrix matrix(state);

    TestLFOSource lfo;
    matrix.add_source("lfo1", &lfo);
    matrix.get_smart_handle<float>("freq");
    matrix.prepare(k_sample_rate, k_block_size);

    // Legacy preset with no priority field.
    nlohmann::json json = nlohmann::json::array();
    json.push_back({{"source_id", "lfo1"},
                    {"target_id", "freq"},
                    {"depth", 0.5f},
                    {"depth_mode", "normalized"},
                    {"combine_mode", "replace"},
                    {"max_decimation", 0}});
    matrix.from_json(json);

    auto round = matrix.to_json(false);
    ASSERT_EQ(round.size(), 1u);
    EXPECT_FALSE(round[0].contains("priority"));
}

TEST(PolyphonicModulation, ToJson_IncludeState) {
    thl::State state;
    state.create("freq", modulatable_float(0.5f));
    ModulationMatrix matrix(state);

    TestLFOSource lfo;
    matrix.add_source("lfo1", &lfo);
    matrix.get_smart_handle<float>("freq");
    matrix.add_routing({"lfo1", "freq", 1.0f});
    matrix.prepare(k_sample_rate, k_block_size);

    auto json = matrix.to_json(true);
    EXPECT_TRUE(json.contains("parameters"));
    EXPECT_TRUE(json.contains("modulation_routings"));
    EXPECT_TRUE(json["parameters"].is_array());
    EXPECT_TRUE(json["modulation_routings"].is_array());
}

// =============================================================================
// Lifecycle tests
// =============================================================================

TEST(PolyphonicModulation, PolySource_DrivesTargetVoiceCount) {
    thl::State state;
    state.create("freq", modulatable_float(0.5f));
    ModulationMatrix matrix(state);

    PolyTestSource poly(4);
    poly.m_voice_values = {0.1f, 0.2f, 0.3f, 0.4f};
    matrix.add_source("poly", &poly);
    matrix.get_smart_handle<float>("freq");
    matrix.add_routing({"poly", "freq", 1.0f});
    matrix.prepare(k_sample_rate, k_block_size);

    const auto* target = matrix.get_target("freq");
    ASSERT_NE(voice_of(target), nullptr);
    EXPECT_EQ(voice_of(target)->m_num_voices, 4u);
}

TEST(PolyphonicModulation, ZeroVoices_NoPolyAllocation) {
    thl::State state;
    state.create("freq", modulatable_float(0.5f));
    ModulationMatrix matrix(state);

    PolyTestSource poly(0);
    poly.m_voice_values = {};  // 0 voices
    matrix.add_source("poly", &poly);
    matrix.get_smart_handle<float>("freq");
    matrix.add_routing({"poly", "freq", 1.0f});
    matrix.prepare(k_sample_rate, k_block_size);

    // 0 voices means no poly allocation
    const auto* target = matrix.get_target("freq");
    EXPECT_EQ(voice_of(target), nullptr);
}

// =============================================================================
// Active mask tests
// =============================================================================

TEST(PolyphonicModulation, ActiveMask_FullyActive_Default) {
    thl::State state;
    state.create("freq", modulatable_float(0.5f));
    ModulationMatrix matrix(state);

    TestLFOSource lfo;
    lfo.m_frequency = 10.0f;
    lfo.m_decimation = 1;
    matrix.add_source("lfo", &lfo);
    auto handle = matrix.get_smart_handle<float>("freq");
    matrix.add_routing({"lfo", "freq", 1.0f});
    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    // Fully active source applies to all samples — verify non-zero modulation
    bool any_modulated = false;
    for (size_t i = 0; i < k_block_size; ++i) {
        if (handle.load(static_cast<uint32_t>(i)) != 0.5f) {
            any_modulated = true;
            break;
        }
    }
    EXPECT_TRUE(any_modulated);
}

TEST(PolyphonicModulation, ActiveMask_Additive_InactiveSkipped) {
    thl::State state;
    state.create("freq", modulatable_float(0.5f));
    ModulationMatrix matrix(state);

    PartiallyActiveSource src;
    src.m_value = 0.3f;
    // Only even samples are active
    src.m_active_pattern.assign(k_block_size, false);
    for (size_t i = 0; i < k_block_size; i += 2) { src.m_active_pattern[i] = true; }

    matrix.add_source("src", &src);
    auto handle = matrix.get_smart_handle<float>("freq");
    matrix.add_routing({"src", "freq", 1.0f});
    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    // Even samples: base + modulation. Odd samples: base only.
    for (size_t i = 0; i < k_block_size; i += 2) {
        EXPECT_FLOAT_EQ(handle.load(static_cast<uint32_t>(i)), 0.5f + 0.3f)
            << "Active sample " << i << " should be modulated";
    }
    for (size_t i = 1; i < k_block_size; i += 2) {
        EXPECT_FLOAT_EQ(handle.load(static_cast<uint32_t>(i)), 0.5f)
            << "Inactive sample " << i << " should be base value";
    }
}

TEST(PolyphonicModulation, ActiveMask_Replace_InactiveFallsBack) {
    thl::State state;
    state.create("freq", modulatable_float(0.5f));
    ModulationMatrix matrix(state);

    PartiallyActiveSource src;
    src.m_value = 0.9f;
    // Only first half active
    src.m_active_pattern.assign(k_block_size, false);
    for (size_t i = 0; i < k_block_size / 2; ++i) { src.m_active_pattern[i] = true; }

    matrix.add_source("src", &src);
    auto handle = matrix.get_smart_handle<float>("freq");
    matrix.add_routing({"src", "freq", 1.0f, 0, DepthMode::Absolute, CombineMode::Replace});
    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    // Active samples: replaced value (0.9 * depth=1.0 = 0.9)
    EXPECT_FLOAT_EQ(handle.load(0), 0.9f);
    // Inactive samples: fall back to base
    EXPECT_FLOAT_EQ(handle.load(static_cast<uint32_t>(k_block_size / 2)), 0.5f);
}

TEST(PolyphonicModulation, ActiveMask_PolyToPoly_PerVoice) {
    thl::State state;
    state.create("freq", modulatable_float(0.5f));
    ModulationMatrix matrix(state);

    PartiallyActivePolySource src(2);
    src.m_voice_values = {0.3f, 0.7f};
    // Voice 0: only even samples active. Voice 1: only odd samples active.
    src.m_voice_active_patterns = {
        std::vector<bool>(k_block_size, false),
        std::vector<bool>(k_block_size, false),
    };
    for (size_t i = 0; i < k_block_size; i += 2) { src.m_voice_active_patterns[0][i] = true; }
    for (size_t i = 1; i < k_block_size; i += 2) { src.m_voice_active_patterns[1][i] = true; }

    matrix.add_source("src", &src);
    matrix.get_smart_handle<float>("freq");
    matrix.add_routing({"src", "freq", 1.0f});
    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    const auto* target = matrix.get_target("freq");
    ASSERT_NE(voice_of(target), nullptr);

    // Voice 0: even samples modulated, odd samples zero
    EXPECT_FLOAT_EQ(voice_of(target)->additive_voice(0)[0], 0.3f);
    EXPECT_FLOAT_EQ(voice_of(target)->additive_voice(0)[1], 0.0f);
    // Voice 1: odd samples modulated, even samples zero
    EXPECT_FLOAT_EQ(voice_of(target)->additive_voice(1)[0], 0.0f);
    EXPECT_FLOAT_EQ(voice_of(target)->additive_voice(1)[1], 0.7f);
}

// =============================================================================
// ReplaceHold tests
// =============================================================================

TEST(PolyphonicModulation, ReplaceHold_HoldsLastValue) {
    thl::State state;
    state.create("freq", modulatable_float(0.5f));
    ModulationMatrix matrix(state);

    PartiallyActiveSource src;
    src.m_value = 0.8f;
    // Only first 10 samples active, rest inactive
    src.m_active_pattern.assign(k_block_size, false);
    for (size_t i = 0; i < 10; ++i) { src.m_active_pattern[i] = true; }

    matrix.add_source("src", &src);
    auto handle = matrix.get_smart_handle<float>("freq");
    matrix.add_routing({"src", "freq", 1.0f, 0, DepthMode::Absolute, CombineMode::ReplaceHold});
    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    // Active samples: replaced with 0.8
    EXPECT_FLOAT_EQ(handle.load(0), 0.8f);
    EXPECT_FLOAT_EQ(handle.load(9), 0.8f);
    // Inactive samples: ReplaceHold should hold the last active value (0.8)
    EXPECT_FLOAT_EQ(handle.load(10), 0.8f);
    EXPECT_FLOAT_EQ(handle.load(static_cast<uint32_t>(k_block_size - 1)), 0.8f);
}

TEST(PolyphonicModulation, ReplaceHold_VsReplace_SameSource) {
    thl::State state;
    state.create("cutoff", modulatable_float(0.5f));
    state.create("play", modulatable_float(0.0f));
    ModulationMatrix matrix(state);

    PartiallyActiveSource src;
    src.m_value = 0.9f;
    // Only first half active
    src.m_active_pattern.assign(k_block_size, false);
    for (size_t i = 0; i < k_block_size / 2; ++i) { src.m_active_pattern[i] = true; }

    matrix.add_source("src", &src);
    auto handle_cutoff = matrix.get_smart_handle<float>("cutoff");
    auto handle_play = matrix.get_smart_handle<float>("play");
    // ReplaceHold on cutoff — holds when inactive
    matrix.add_routing({"src", "cutoff", 1.0f, 0, DepthMode::Absolute, CombineMode::ReplaceHold});
    // Replace on play — falls back to base when inactive
    matrix.add_routing({"src", "play", 1.0f, 0, DepthMode::Absolute, CombineMode::Replace});
    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    const auto inactive_idx = static_cast<uint32_t>(k_block_size / 2 + 1);
    // Cutoff: held at last active value
    EXPECT_FLOAT_EQ(handle_cutoff.load(inactive_idx), 0.9f);
    // Play: falls back to base
    EXPECT_FLOAT_EQ(handle_play.load(inactive_idx), 0.0f);
}

// =============================================================================
// Replace-only poly target changepoint propagation (bug fix test)
// =============================================================================

TEST(PolyphonicModulation, ChangePoints_PolyReplace_Propagated) {
    thl::State state;
    state.create("freq", modulatable_float(0.5f));
    ModulationMatrix matrix(state);

    PolyTestSource poly(2);
    poly.m_voice_values = {0.3f, 0.7f};
    matrix.add_source("poly", &poly);
    matrix.get_smart_handle<float>("freq");
    matrix.add_routing({"poly", "freq", 1.0f, 0, DepthMode::Absolute, CombineMode::Replace});
    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    const auto* target = matrix.get_target("freq");
    ASSERT_NE(voice_of(target), nullptr);
    // Replace-only poly target should still have changepoints propagated
    EXPECT_FALSE(voice_of(target)->m_change_points[0].empty());
    EXPECT_FALSE(voice_of(target)->m_change_points[1].empty());
}

TEST(PolyphonicModulation, CollectChangePoints_IncludesReplace) {
    thl::State state;
    state.create("freq", modulatable_float(0.5f));
    ModulationMatrix matrix(state);

    PolyTestSource poly(2);
    poly.m_voice_values = {0.3f, 0.7f};
    matrix.add_source("poly", &poly);
    auto handle = matrix.get_smart_handle<float>("freq");
    matrix.add_routing({"poly", "freq", 1.0f, 0, DepthMode::Absolute, CombineMode::Replace});
    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    // collect_change_points should include replace-only voice changepoints
    std::array<SmartHandle<float>, 1> handles = {handle};
    std::vector<uint32_t> change_points;
    change_points.reserve(k_block_size);
    collect_change_points(std::span<const SmartHandle<float>>(handles), change_points, 0);
    EXPECT_FALSE(change_points.empty());
}
