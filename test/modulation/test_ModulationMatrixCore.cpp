#include <gtest/gtest.h>

#include "TestHelpers.h"

#include <nlohmann/json.hpp>

#include <tanh/modulation/ModulationMatrix.h>
#include <tanh/state/State.h>

using namespace thl::modulation;

// Constant-value source for deterministic tests.
class ConstSource : public ModulationSource {
public:
    float m_value = 1.0f;
    ConstSource() : ModulationSource(true, 0, true) {}
    void prepare(double /*sr*/, size_t spb) override { resize_buffers(spb); }
    void process(size_t num_samples, size_t offset = 0) override {
        for (size_t i = offset; i < offset + num_samples; ++i) { m_output_buffer[i] = m_value; }
        if (num_samples > 0) { record_change_point(static_cast<uint32_t>(offset)); }
    }
};

TEST(ModulationMatrix, SingleSourceSingleTarget) {
    thl::State state;
    state.create("freq", modulatable_float(0.0f));
    ModulationMatrix matrix(state);
    TestLFOSource lfo;
    lfo.m_frequency = 1.0f;
    lfo.m_waveform = LFOWaveform::Sine;

    matrix.add_source("lfo1", &lfo);
    matrix.get_smart_handle<float>("freq");
    matrix.add_routing({"lfo1", "freq", 100.0f});

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    const auto* target = matrix.get_target("freq");
    ASSERT_NE(target, nullptr);

    // Modulation buffer should have non-zero values (sine LFO * depth 100)
    bool has_nonzero = false;
    for (size_t i = 0; i < k_block_size; ++i) {
        if (target->m_additive_buffer[i] != 0.0f) {
            has_nonzero = true;
            break;
        }
    }
    EXPECT_TRUE(has_nonzero);

    // Change points should exist
    EXPECT_FALSE(target->m_change_points.empty());
}

TEST(ModulationMatrix, MultipleSourcesSameTarget) {
    thl::State state;
    state.create("freq", modulatable_float(0.0f));
    ModulationMatrix matrix(state);

    TestLFOSource lfo1;
    lfo1.m_frequency = 1.0f;
    lfo1.m_waveform = LFOWaveform::Sine;

    TestLFOSource lfo2;
    lfo2.m_frequency = 5.0f;
    lfo2.m_waveform = LFOWaveform::Triangle;

    matrix.add_source("lfo1", &lfo1);
    matrix.add_source("lfo2", &lfo2);
    matrix.get_smart_handle<float>("freq");
    matrix.add_routing({"lfo1", "freq", 50.0f});
    matrix.add_routing({"lfo2", "freq", 25.0f});

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    const auto* target = matrix.get_target("freq");
    ASSERT_NE(target, nullptr);

    // The modulation buffer should be the sum of both LFO outputs * depths
    const auto& out1 = lfo1.get_output_buffer();
    const auto& out2 = lfo2.get_output_buffer();
    for (size_t i = 0; i < k_block_size; ++i) {
        float expected = out1[i] * 50.0f + out2[i] * 25.0f;
        EXPECT_FLOAT_EQ(target->m_additive_buffer[i], expected) << "Mismatch at sample " << i;
    }
}

TEST(ModulationMatrix, RemoveRouting) {
    thl::State state;
    state.create("freq", modulatable_float(0.0f));
    ModulationMatrix matrix(state);
    TestLFOSource lfo;
    lfo.m_frequency = 1.0f;

    matrix.add_source("lfo1", &lfo);
    matrix.get_smart_handle<float>("freq");
    matrix.add_routing({"lfo1", "freq", 100.0f});

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    // Note there's modulation
    const auto* target = matrix.get_target("freq");
    ASSERT_NE(target, nullptr);
    bool has_nonzero = false;
    for (size_t i = 0; i < k_block_size; ++i) {
        if (target->m_additive_buffer[i] != 0.0f) {
            has_nonzero = true;
            break;
        }
    }
    EXPECT_TRUE(has_nonzero);

    // Remove routing and process again
    matrix.remove_routing("lfo1", "freq");
    matrix.process(k_block_size);

    target = matrix.get_target("freq");
    ASSERT_NE(target, nullptr);
    // No routings → sparse allocation clears the buffer
    EXPECT_FALSE(target->m_has_mono_additive);
    EXPECT_TRUE(target->m_additive_buffer.empty());
}

TEST(ModulationMatrix, PerRoutingDecimation) {
    thl::State state;
    state.create("freq", modulatable_float(0.0f));
    ModulationMatrix matrix(state);
    TestLFOSource lfo;
    lfo.m_frequency = 10.0f;
    lfo.m_decimation = 1;  // Source has max resolution

    matrix.add_source("lfo1", &lfo);
    matrix.get_smart_handle<float>("freq");

    ModulationRouting routing;
    routing.m_source_id = "lfo1";
    routing.m_target_id = "freq";
    routing.m_depth = 1.0f;
    routing.m_max_decimation = 32;
    matrix.add_routing(routing);

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    const auto* target = matrix.get_target("freq");
    ASSERT_NE(target, nullptr);

    // With max_decimation = 32, there should be at least
    // kBlockSize / 32 change points from the routing alone
    EXPECT_GE(target->m_change_points.size(), k_block_size / 32);
}

TEST(ModulationMatrix, UnresolvableRoutingRejected) {
    thl::State state;
    ModulationMatrix matrix(state);
    TestLFOSource lfo;
    lfo.m_frequency = 1.0f;

    matrix.add_source("lfo1", &lfo);
    // Target parameter does not exist in State — routing must be rejected
    const uint32_t id = matrix.add_routing({"lfo1", "nonexistent", 100.0f});
    EXPECT_EQ(id, k_invalid_routing_id);

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);
}

TEST(ModulationMatrix, AddRoutingRejectsUnregisteredSource) {
    thl::State state;
    state.create("freq", modulatable_float(0.0f));
    ModulationMatrix matrix(state);

    const uint32_t id = matrix.add_routing({"no_such_source", "freq", 1.0f});
    EXPECT_EQ(id, k_invalid_routing_id);
}

TEST(ModulationMatrix, AddRoutingRejectsNonModulatableTarget) {
    thl::State state;
    // Create a parameter without the modulatable flag
    state.create("fixed_param",
                 thl::ParameterDefinition::make_float("", thl::Range::linear(0.0f, 1.0f), 0.5f));
    ModulationMatrix matrix(state);
    TestLFOSource lfo;

    matrix.add_source("lfo1", &lfo);
    const uint32_t id = matrix.add_routing({"lfo1", "fixed_param", 1.0f});
    EXPECT_EQ(id, k_invalid_routing_id);
}

TEST(ModulationMatrix, AddRoutingResolvesTarget) {
    thl::State state;
    state.create("freq", modulatable_float(0.0f));
    ModulationMatrix matrix(state);
    TestLFOSource lfo;
    lfo.m_frequency = 1.0f;
    lfo.m_waveform = LFOWaveform::Sine;

    matrix.add_source("lfo1", &lfo);
    // add_routing without prior get_smart_handle — target should be auto-resolved
    matrix.add_routing({"lfo1", "freq", 100.0f});

    const auto* target = matrix.get_target("freq");
    ASSERT_NE(target, nullptr);

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    // Target should have modulation data
    bool has_nonzero = false;
    for (size_t i = 0; i < k_block_size; ++i) {
        if (target->m_additive_buffer[i] != 0.0f) {
            has_nonzero = true;
            break;
        }
    }
    EXPECT_TRUE(has_nonzero);
}

TEST(ModulationMatrix, UnroutedTargetNotInActiveTargets) {
    thl::State state;
    state.create("routed", modulatable_float(0.0f));
    state.create("unrouted", modulatable_float(0.0f));
    ModulationMatrix matrix(state);
    TestLFOSource lfo;
    lfo.m_frequency = 1.0f;

    matrix.add_source("lfo1", &lfo);
    matrix.get_smart_handle<float>("routed");
    matrix.get_smart_handle<float>("unrouted");
    matrix.add_routing({"lfo1", "routed", 100.0f});

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    // Routed target should have modulation data
    const auto* routed = matrix.get_target("routed");
    ASSERT_NE(routed, nullptr);
    EXPECT_FALSE(routed->m_additive_buffer.empty());

    // Unrouted target should have empty buffers (not processed)
    const auto* unrouted = matrix.get_target("unrouted");
    ASSERT_NE(unrouted, nullptr);
    EXPECT_FALSE(unrouted->m_has_mono_additive);
    EXPECT_FALSE(unrouted->m_has_mono_replace);
    EXPECT_TRUE(unrouted->m_additive_buffer.empty());
    EXPECT_TRUE(unrouted->m_change_point_flags.empty());
}

TEST(ModulationMatrix, RoutingRemovedTargetBecomesInactive) {
    thl::State state;
    state.create("freq", modulatable_float(0.0f));
    ModulationMatrix matrix(state);
    TestLFOSource lfo;
    lfo.m_frequency = 1.0f;

    matrix.add_source("lfo1", &lfo);
    matrix.get_smart_handle<float>("freq");
    matrix.add_routing({"lfo1", "freq", 100.0f});

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    const auto* target = matrix.get_target("freq");
    ASSERT_NE(target, nullptr);
    EXPECT_FALSE(target->m_additive_buffer.empty());

    // Remove the routing — target should become inactive
    matrix.remove_routing("lfo1", "freq");
    matrix.process(k_block_size);

    EXPECT_FALSE(target->m_has_mono_additive);
    EXPECT_FALSE(target->m_has_mono_replace);
    EXPECT_TRUE(target->m_additive_buffer.empty());
    EXPECT_TRUE(target->m_change_point_flags.empty());
}

// ── Runtime Depth Update Tests ──────────────────────────────────────────────

TEST(ModulationMatrix, UpdateRoutingDepth_ChangesOutput) {
    thl::State state;
    state.create("param", modulatable_float(0.0f));
    ModulationMatrix matrix(state);

    ConstSource src;
    src.m_value = 1.0f;

    matrix.add_source("src", &src);
    matrix.get_smart_handle<float>("param");
    matrix.add_routing({"src", "param", 0.5f, 0, DepthMode::Normalized});

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    const auto* target = matrix.get_target("param");
    ASSERT_NE(target, nullptr);
    // depth 0.5 on range [0,1] → precomputed = 0.5 * 1.0 = 0.5, buffer = 1.0 * 0.5 = 0.5
    EXPECT_FLOAT_EQ(target->m_additive_buffer[0], 0.5f);

    // Update depth to 0.25 without schedule rebuild.
    EXPECT_TRUE(matrix.update_routing_depth("src", "param", 0.25f));
    matrix.process(k_block_size);

    EXPECT_FLOAT_EQ(target->m_additive_buffer[0], 0.25f);
}

TEST(ModulationMatrix, UpdateRoutingDepth_SurvivesRebuild) {
    thl::State state;
    state.create("param", modulatable_float(0.0f));
    ModulationMatrix matrix(state);

    ConstSource src;
    src.m_value = 1.0f;

    matrix.add_source("src", &src);
    matrix.get_smart_handle<float>("param");
    matrix.add_routing({"src", "param", 0.5f, 0, DepthMode::Normalized});

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    // Update depth, then trigger a full schedule rebuild.
    EXPECT_TRUE(matrix.update_routing_depth("src", "param", 0.75f));
    matrix.rebuild_schedule();
    matrix.process(k_block_size);

    const auto* target = matrix.get_target("param");
    ASSERT_NE(target, nullptr);
    EXPECT_FLOAT_EQ(target->m_additive_buffer[0], 0.75f);
}

TEST(ModulationMatrix, UpdateRoutingDepth_NotFound) {
    thl::State state;
    ModulationMatrix matrix(state);
    EXPECT_FALSE(matrix.update_routing_depth("nonexistent", "nonexistent", 1.0f));
}

// ── Replace Range Tests ─────────────────────────────────────────────────────

TEST(ModulationMatrix, ReplaceRange_MapsSourceToRange) {
    thl::State state;
    state.create(
        "freq",
        thl::ParameterDefinition::make_float("Freq", thl::Range::linear(0.0f, 1000.0f), 500.0f)
            .modulatable(true));
    ModulationMatrix matrix(state);

    ConstSource src;
    src.m_value = 0.5f;

    matrix.add_source("src", &src);
    auto handle = matrix.get_smart_handle<float>("freq");

    ModulationRouting routing;
    routing.m_source_id = "src";
    routing.m_target_id = "freq";
    routing.m_depth = 1.0f;
    routing.m_combine_mode = CombineMode::Replace;
    routing.m_replace_range_min = 200.0f;
    routing.m_replace_range_max = 800.0f;
    routing.m_has_replace_range = true;
    matrix.add_routing(routing);

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    // value = 200 + 0.5 * 1.0 * (800 - 200) = 200 + 300 = 500
    EXPECT_FLOAT_EQ(handle.load(0), 500.0f);
}

TEST(ModulationMatrix, ReplaceRange_DepthScalesSource) {
    thl::State state;
    state.create(
        "freq",
        thl::ParameterDefinition::make_float("Freq", thl::Range::linear(0.0f, 1000.0f), 500.0f)
            .modulatable(true));
    ModulationMatrix matrix(state);

    ConstSource src;
    src.m_value = 1.0f;

    matrix.add_source("src", &src);
    auto handle = matrix.get_smart_handle<float>("freq");

    ModulationRouting routing;
    routing.m_source_id = "src";
    routing.m_target_id = "freq";
    routing.m_depth = 0.5f;
    routing.m_combine_mode = CombineMode::Replace;
    routing.m_replace_range_min = 200.0f;
    routing.m_replace_range_max = 800.0f;
    routing.m_has_replace_range = true;
    matrix.add_routing(routing);

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    // value = 200 + 1.0 * 0.5 * (800 - 200) = 200 + 300 = 500
    EXPECT_FLOAT_EQ(handle.load(0), 500.0f);
}

TEST(ModulationMatrix, UpdateReplaceRange_Runtime) {
    thl::State state;
    state.create(
        "freq",
        thl::ParameterDefinition::make_float("Freq", thl::Range::linear(0.0f, 1000.0f), 500.0f)
            .modulatable(true));
    ModulationMatrix matrix(state);

    ConstSource src;
    src.m_value = 1.0f;

    matrix.add_source("src", &src);
    auto handle = matrix.get_smart_handle<float>("freq");

    ModulationRouting routing;
    routing.m_source_id = "src";
    routing.m_target_id = "freq";
    routing.m_depth = 1.0f;
    routing.m_combine_mode = CombineMode::Replace;
    routing.m_replace_range_min = 200.0f;
    routing.m_replace_range_max = 800.0f;
    routing.m_has_replace_range = true;
    matrix.add_routing(routing);

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    // Initial: value = 200 + 1.0 * 1.0 * 600 = 800
    EXPECT_FLOAT_EQ(handle.load(0), 800.0f);

    // Update range at runtime without schedule rebuild.
    EXPECT_TRUE(matrix.update_routing_replace_range("src", "freq", 100.0f, 500.0f));
    matrix.process(k_block_size);

    // New: value = 100 + 1.0 * 1.0 * 400 = 500
    EXPECT_FLOAT_EQ(handle.load(0), 500.0f);
}

TEST(ModulationMatrix, ClearReplaceRange_RevertsToDefault) {
    thl::State state;
    state.create("param",
                 thl::ParameterDefinition::make_float("P", thl::Range::linear(0.0f, 100.0f), 50.0f)
                     .modulatable(true));
    ModulationMatrix matrix(state);

    ConstSource src;
    src.m_value = 0.5f;

    matrix.add_source("src", &src);
    auto handle = matrix.get_smart_handle<float>("param");

    ModulationRouting routing;
    routing.m_source_id = "src";
    routing.m_target_id = "param";
    routing.m_depth = 1.0f;
    routing.m_depth_mode = DepthMode::Absolute;
    routing.m_combine_mode = CombineMode::Replace;
    routing.m_replace_range_min = 10.0f;
    routing.m_replace_range_max = 90.0f;
    routing.m_has_replace_range = true;
    matrix.add_routing(routing);

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    // With range: value = 10 + 0.5 * 1.0 * 80 = 50
    EXPECT_FLOAT_EQ(handle.load(0), 50.0f);

    // Clear range — revert to src * depth_precomputed behavior.
    EXPECT_TRUE(matrix.clear_routing_replace_range("src", "param"));
    matrix.process(k_block_size);

    // Without range: value = src * depth = 0.5 * 1.0 = 0.5 (absolute depth on linear)
    EXPECT_FLOAT_EQ(handle.load(0), 0.5f);
}

TEST(ModulationMatrix, Serialization_ReplaceRangeRoundTrip) {
    thl::State state;
    state.create("freq", modulatable_float(0.5f));
    ModulationMatrix matrix(state);

    ConstSource src;
    matrix.add_source("src", &src);
    matrix.get_smart_handle<float>("freq");

    ModulationRouting routing;
    routing.m_source_id = "src";
    routing.m_target_id = "freq";
    routing.m_depth = 0.75f;
    routing.m_combine_mode = CombineMode::Replace;
    routing.m_replace_range_min = 100.0f;
    routing.m_replace_range_max = 900.0f;
    routing.m_has_replace_range = true;
    matrix.add_routing(routing);
    matrix.prepare(k_sample_rate, k_block_size);

    auto json = matrix.to_json(false);
    ASSERT_TRUE(json.is_array());
    ASSERT_EQ(json.size(), 1u);
    EXPECT_FLOAT_EQ(json[0]["replace_range_min"].get<float>(), 100.0f);
    EXPECT_FLOAT_EQ(json[0]["replace_range_max"].get<float>(), 900.0f);

    // Restore into a new matrix.
    ModulationMatrix matrix2(state);
    matrix2.add_source("src", &src);
    matrix2.get_smart_handle<float>("freq");
    matrix2.from_json(json);
    matrix2.prepare(k_sample_rate, k_block_size);

    auto json2 = matrix2.to_json(false);
    ASSERT_EQ(json2.size(), 1u);
    EXPECT_FLOAT_EQ(json2[0]["replace_range_min"].get<float>(), 100.0f);
    EXPECT_FLOAT_EQ(json2[0]["replace_range_max"].get<float>(), 900.0f);
}

// ── Routing ID Tests ────────────────────────────────────────────────────────

TEST(ModulationMatrix, AddRouting_ReturnsUniqueIDs) {
    thl::State state;
    state.create("p1", modulatable_float(0.0f));
    state.create("p2", modulatable_float(0.0f));
    ModulationMatrix matrix(state);

    ConstSource src;
    matrix.add_source("src", &src);
    matrix.get_smart_handle<float>("p1");
    matrix.get_smart_handle<float>("p2");

    uint32_t id1 = matrix.add_routing({"src", "p1", 1.0f});
    uint32_t id2 = matrix.add_routing({"src", "p2", 1.0f});

    EXPECT_NE(id1, k_invalid_routing_id);
    EXPECT_NE(id2, k_invalid_routing_id);
    EXPECT_NE(id1, id2);
}

TEST(ModulationMatrix, AddRouting_RejectsDuplicateSourceTarget) {
    thl::State state;
    state.create("param", modulatable_float(0.0f));
    ModulationMatrix matrix(state);

    ConstSource src;
    matrix.add_source("src", &src);
    matrix.get_smart_handle<float>("param");

    uint32_t id1 = matrix.add_routing({"src", "param", 1.0f});
    uint32_t id2 = matrix.add_routing({"src", "param", 0.5f});

    EXPECT_NE(id1, k_invalid_routing_id);
    EXPECT_EQ(id2, k_invalid_routing_id);
}

TEST(ModulationMatrix, RemoveRouting_ById) {
    thl::State state;
    state.create("p1", modulatable_float(0.0f));
    state.create("p2", modulatable_float(0.0f));
    ModulationMatrix matrix(state);

    ConstSource src;
    src.m_value = 1.0f;
    matrix.add_source("src", &src);
    matrix.get_smart_handle<float>("p1");
    matrix.get_smart_handle<float>("p2");

    uint32_t id1 = matrix.add_routing({"src", "p1", 1.0f});
    uint32_t id2 = matrix.add_routing({"src", "p2", 1.0f});

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    // Both targets should have modulation.
    ASSERT_NE(matrix.get_target("p1"), nullptr);
    ASSERT_NE(matrix.get_target("p2"), nullptr);
    EXPECT_TRUE(matrix.get_target("p1")->m_has_mono_additive);
    EXPECT_TRUE(matrix.get_target("p2")->m_has_mono_additive);

    // Remove only the first routing by ID.
    matrix.remove_routing(id1);
    matrix.process(k_block_size);

    EXPECT_FALSE(matrix.get_target("p1")->m_has_mono_additive);
    EXPECT_TRUE(matrix.get_target("p2")->m_has_mono_additive);

    // Remove the second.
    matrix.remove_routing(id2);
    matrix.process(k_block_size);
    EXPECT_FALSE(matrix.get_target("p2")->m_has_mono_additive);
}

TEST(ModulationMatrix, UpdateRoutingDepth_ById) {
    thl::State state;
    state.create("param", modulatable_float(0.0f));
    ModulationMatrix matrix(state);

    ConstSource src;
    src.m_value = 1.0f;
    matrix.add_source("src", &src);
    matrix.get_smart_handle<float>("param");

    uint32_t id = matrix.add_routing({"src", "param", 0.5f, 0, DepthMode::Normalized});

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    const auto* target = matrix.get_target("param");
    ASSERT_NE(target, nullptr);
    EXPECT_FLOAT_EQ(target->m_additive_buffer[0], 0.5f);

    // Update by ID.
    EXPECT_TRUE(matrix.update_routing_depth(id, 0.25f));
    matrix.process(k_block_size);
    EXPECT_FLOAT_EQ(target->m_additive_buffer[0], 0.25f);

    // Non-existent ID returns false.
    EXPECT_FALSE(matrix.update_routing_depth(uint32_t{9999}, 1.0f));
}

TEST(ModulationMatrix, UpdateReplaceRange_ById) {
    thl::State state;
    state.create(
        "freq",
        thl::ParameterDefinition::make_float("Freq", thl::Range::linear(0.0f, 1000.0f), 500.0f)
            .modulatable(true));
    ModulationMatrix matrix(state);

    ConstSource src;
    src.m_value = 1.0f;
    matrix.add_source("src", &src);
    auto handle = matrix.get_smart_handle<float>("freq");

    ModulationRouting routing;
    routing.m_source_id = "src";
    routing.m_target_id = "freq";
    routing.m_depth = 1.0f;
    routing.m_combine_mode = CombineMode::Replace;
    routing.m_replace_range_min = 200.0f;
    routing.m_replace_range_max = 800.0f;
    routing.m_has_replace_range = true;
    uint32_t id = matrix.add_routing(routing);

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    // Initial: 200 + 1.0 * 1.0 * 600 = 800
    EXPECT_FLOAT_EQ(handle.load(0), 800.0f);

    // Update range by ID.
    EXPECT_TRUE(matrix.update_routing_replace_range(id, 100.0f, 500.0f));
    matrix.process(k_block_size);

    // New: 100 + 1.0 * 1.0 * 400 = 500
    EXPECT_FLOAT_EQ(handle.load(0), 500.0f);

    // Clear range by ID.
    EXPECT_TRUE(matrix.clear_routing_replace_range(id));
    matrix.process(k_block_size);

    // Without range: src * depth_precomputed = 1.0 * 1000.0 = 1000
    EXPECT_FLOAT_EQ(handle.load(0), 1000.0f);

    // Non-existent ID returns false.
    EXPECT_FALSE(matrix.update_routing_replace_range(uint32_t{9999}, 0.0f, 1.0f));
    EXPECT_FALSE(matrix.clear_routing_replace_range(uint32_t{9999}));
}

TEST(ModulationMatrix, Serialization_RoutingIdRoundTrip) {
    thl::State state;
    state.create("p1", modulatable_float(0.0f));
    state.create("p2", modulatable_float(0.0f));
    ModulationMatrix matrix(state);

    ConstSource src;
    matrix.add_source("src", &src);
    matrix.get_smart_handle<float>("p1");
    matrix.get_smart_handle<float>("p2");

    uint32_t id1 = matrix.add_routing({"src", "p1", 0.5f});
    uint32_t id2 = matrix.add_routing({"src", "p2", 0.75f});
    matrix.prepare(k_sample_rate, k_block_size);

    // Serialize.
    auto json = matrix.to_json(false);
    ASSERT_TRUE(json.is_array());
    ASSERT_EQ(json.size(), 2u);
    EXPECT_EQ(json[0]["id"].get<uint32_t>(), id1);
    EXPECT_EQ(json[1]["id"].get<uint32_t>(), id2);

    // Deserialize into a new matrix — IDs should be preserved.
    ModulationMatrix matrix2(state);
    matrix2.add_source("src", &src);
    matrix2.get_smart_handle<float>("p1");
    matrix2.get_smart_handle<float>("p2");
    matrix2.from_json(json);
    matrix2.prepare(k_sample_rate, k_block_size);

    auto json2 = matrix2.to_json(false);
    ASSERT_EQ(json2.size(), 2u);
    EXPECT_EQ(json2[0]["id"].get<uint32_t>(), id1);
    EXPECT_EQ(json2[1]["id"].get<uint32_t>(), id2);

    // Adding a new routing after deserialization should get a higher ID.
    state.create("p3", modulatable_float(0.0f));
    matrix2.get_smart_handle<float>("p3");
    uint32_t id3 = matrix2.add_routing({"src", "p3", 1.0f});
    EXPECT_GT(id3, id2);
}

// ── Skip During Gesture Tests ───────────────────────────────────────────────

TEST(ModulationMatrix, SkipDuringGesture_SuppressesModulation) {
    thl::State state;
    state.create("freq", modulatable_float(0.0f));
    ModulationMatrix matrix(state);

    ConstSource src;
    src.m_value = 1.0f;
    matrix.add_source("src", &src);
    matrix.get_smart_handle<float>("freq");

    ModulationRouting routing;
    routing.m_source_id = "src";
    routing.m_target_id = "freq";
    routing.m_depth = 1.0f;
    routing.m_skip_during_gesture = true;
    matrix.add_routing(routing);

    matrix.prepare(k_sample_rate, k_block_size);

    // No gesture — modulation should be applied.
    matrix.process(k_block_size);
    const auto* target = matrix.get_target("freq");
    ASSERT_NE(target, nullptr);
    EXPECT_TRUE(target->m_has_mono_additive);
    EXPECT_FLOAT_EQ(target->m_additive_buffer[0], 1.0f);

    // Begin gesture — modulation should be suppressed (buffer stays zero).
    state.set_gesture_from_root("freq", true);
    matrix.process(k_block_size);
    for (size_t i = 0; i < k_block_size; ++i) {
        EXPECT_FLOAT_EQ(target->m_additive_buffer[i], 0.0f) << "Sample " << i;
    }

    // End gesture — modulation should resume.
    state.set_gesture_from_root("freq", false);
    matrix.process(k_block_size);
    EXPECT_TRUE(target->m_has_mono_additive);
    EXPECT_FLOAT_EQ(target->m_additive_buffer[0], 1.0f);
}

TEST(ModulationMatrix, SkipDuringGesture_DefaultDoesNotSuppress) {
    thl::State state;
    state.create("freq", modulatable_float(0.0f));
    ModulationMatrix matrix(state);

    ConstSource src;
    src.m_value = 1.0f;
    matrix.add_source("src", &src);
    matrix.get_smart_handle<float>("freq");
    matrix.add_routing({"src", "freq", 1.0f});

    matrix.prepare(k_sample_rate, k_block_size);

    // Gesture active, but default routing (skip_during_gesture = false).
    state.set_gesture_from_root("freq", true);
    matrix.process(k_block_size);

    const auto* target = matrix.get_target("freq");
    ASSERT_NE(target, nullptr);
    EXPECT_TRUE(target->m_has_mono_additive);
    EXPECT_FLOAT_EQ(target->m_additive_buffer[0], 1.0f);
}

TEST(ModulationMatrix, SkipDuringGesture_OnlyAffectsRoutingsWithFlag) {
    thl::State state;
    state.create("freq", modulatable_float(0.0f));
    ModulationMatrix matrix(state);

    ConstSource src1;
    src1.m_value = 1.0f;
    ConstSource src2;
    src2.m_value = 1.0f;

    matrix.add_source("src1", &src1);
    matrix.add_source("src2", &src2);
    matrix.get_smart_handle<float>("freq");

    // src1 routing: skip during gesture, depth 0.5
    ModulationRouting r1;
    r1.m_source_id = "src1";
    r1.m_target_id = "freq";
    r1.m_depth = 0.5f;
    r1.m_skip_during_gesture = true;
    matrix.add_routing(r1);

    // src2 routing: does NOT skip, depth 0.25
    matrix.add_routing({"src2", "freq", 0.25f});

    matrix.prepare(k_sample_rate, k_block_size);

    // No gesture — both routings contribute.
    matrix.process(k_block_size);
    const auto* target = matrix.get_target("freq");
    ASSERT_NE(target, nullptr);
    EXPECT_FLOAT_EQ(target->m_additive_buffer[0], 0.75f);

    // During gesture — only src2 contributes.
    state.set_gesture_from_root("freq", true);
    matrix.process(k_block_size);
    EXPECT_FLOAT_EQ(target->m_additive_buffer[0], 0.25f);
}

TEST(ModulationMatrix, SkipDuringGesture_ChangePointsNotPropagated) {
    thl::State state;
    state.create("freq", modulatable_float(0.0f));
    ModulationMatrix matrix(state);

    TestLFOSource lfo;
    lfo.m_frequency = 10.0f;
    lfo.m_waveform = LFOWaveform::Sine;

    matrix.add_source("lfo", &lfo);
    matrix.get_smart_handle<float>("freq");

    ModulationRouting routing;
    routing.m_source_id = "lfo";
    routing.m_target_id = "freq";
    routing.m_depth = 1.0f;
    routing.m_skip_during_gesture = true;
    matrix.add_routing(routing);

    matrix.prepare(k_sample_rate, k_block_size);

    // No gesture — change points should exist.
    matrix.process(k_block_size);
    const auto* target = matrix.get_target("freq");
    ASSERT_NE(target, nullptr);
    EXPECT_FALSE(target->m_change_points.empty());

    // During gesture — no change points.
    state.set_gesture_from_root("freq", true);
    matrix.process(k_block_size);
    EXPECT_TRUE(target->m_change_points.empty());
}

TEST(ModulationMatrix, SkipDuringGesture_SerializationRoundTrip) {
    thl::State state;
    state.create("freq", modulatable_float(0.0f));
    ModulationMatrix matrix(state);

    ConstSource src;
    matrix.add_source("src", &src);
    matrix.get_smart_handle<float>("freq");

    ModulationRouting routing;
    routing.m_source_id = "src";
    routing.m_target_id = "freq";
    routing.m_depth = 1.0f;
    routing.m_skip_during_gesture = true;
    matrix.add_routing(routing);
    matrix.prepare(k_sample_rate, k_block_size);

    // Serialize — flag should be present.
    auto json = matrix.to_json(false);
    ASSERT_TRUE(json.is_array());
    ASSERT_EQ(json.size(), 1u);
    EXPECT_TRUE(json[0]["skip_during_gesture"].get<bool>());

    // Deserialize into new matrix.
    ModulationMatrix matrix2(state);
    matrix2.add_source("src", &src);
    matrix2.get_smart_handle<float>("freq");
    matrix2.from_json(json);
    matrix2.prepare(k_sample_rate, k_block_size);

    // Re-serialize — flag should survive.
    auto json2 = matrix2.to_json(false);
    ASSERT_EQ(json2.size(), 1u);
    EXPECT_TRUE(json2[0]["skip_during_gesture"].get<bool>());

    // Default routing should NOT have the key in JSON.
    ModulationMatrix matrix3(state);
    matrix3.add_source("src", &src);
    matrix3.get_smart_handle<float>("freq");
    matrix3.add_routing({"src", "freq", 1.0f});
    matrix3.prepare(k_sample_rate, k_block_size);

    auto json3 = matrix3.to_json(false);
    ASSERT_EQ(json3.size(), 1u);
    EXPECT_FALSE(json3[0].contains("skip_during_gesture"));
}
