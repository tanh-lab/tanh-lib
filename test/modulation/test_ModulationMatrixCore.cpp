#include <gtest/gtest.h>

#include <array>
#include <utility>

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
        if (mono_of(target)->m_additive_buffer[i] != 0.0f) {
            has_nonzero = true;
            break;
        }
    }
    EXPECT_TRUE(has_nonzero);

    // Change points should exist
    EXPECT_FALSE(mono_of(target)->m_change_points.empty());
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
        EXPECT_FLOAT_EQ(mono_of(target)->m_additive_buffer[i], expected)
            << "Mismatch at sample " << i;
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
        if (mono_of(target)->m_additive_buffer[i] != 0.0f) {
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
    EXPECT_FALSE(has_mono_additive(target));
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
    EXPECT_GE(mono_of(target)->m_change_points.size(), k_block_size / 32);
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
        if (mono_of(target)->m_additive_buffer[i] != 0.0f) {
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
    EXPECT_TRUE(has_mono_additive(routed));

    // Unrouted target should have empty buffers (not processed)
    const auto* unrouted = matrix.get_target("unrouted");
    ASSERT_NE(unrouted, nullptr);
    EXPECT_FALSE(has_mono_additive(unrouted));
    EXPECT_FALSE(has_mono_replace(unrouted));
    EXPECT_FALSE(has_mono_additive(unrouted));
    EXPECT_EQ(mono_of(unrouted), nullptr);
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
    EXPECT_TRUE(has_mono_additive(target));
    EXPECT_TRUE(has_mono_additive(target));

    // Remove the routing — target should become inactive
    matrix.remove_routing("lfo1", "freq");
    matrix.process(k_block_size);

    EXPECT_FALSE(has_mono_additive(target));
    EXPECT_FALSE(has_mono_replace(target));
    EXPECT_FALSE(has_mono_additive(target));
    EXPECT_EQ(mono_of(target), nullptr);
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
    EXPECT_FLOAT_EQ(mono_of(target)->m_additive_buffer[0], 0.5f);

    // Update depth to 0.25 without schedule rebuild.
    EXPECT_TRUE(matrix.update_routing_depth("src", "param", 0.25f));
    matrix.process(k_block_size);

    EXPECT_FLOAT_EQ(mono_of(target)->m_additive_buffer[0], 0.25f);
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
    EXPECT_FLOAT_EQ(mono_of(target)->m_additive_buffer[0], 0.75f);
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

// Negative depth on a replace routing with an explicit range must reverse the
// direction of the mapping: src=0 → rmax, src=1 → rmin. Regression for a bug
// where inverted full-range mappings collapsed every source value to rmin.
TEST(ModulationMatrix, ReplaceRange_InvertedReversesEndpoints_SrcOne) {
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
    routing.m_depth = -1.0f;  // inverted
    routing.m_combine_mode = CombineMode::Replace;
    routing.m_replace_range_min = 200.0f;
    routing.m_replace_range_max = 800.0f;
    routing.m_has_replace_range = true;
    matrix.add_routing(routing);

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    // src=1 with inverted routing → rmin
    EXPECT_FLOAT_EQ(handle.load(0), 200.0f);
}

TEST(ModulationMatrix, ReplaceRange_InvertedReversesEndpoints_SrcZero) {
    thl::State state;
    state.create(
        "freq",
        thl::ParameterDefinition::make_float("Freq", thl::Range::linear(0.0f, 1000.0f), 500.0f)
            .modulatable(true));
    ModulationMatrix matrix(state);

    ConstSource src;
    src.m_value = 0.0f;

    matrix.add_source("src", &src);
    auto handle = matrix.get_smart_handle<float>("freq");

    ModulationRouting routing;
    routing.m_source_id = "src";
    routing.m_target_id = "freq";
    routing.m_depth = -1.0f;
    routing.m_combine_mode = CombineMode::Replace;
    routing.m_replace_range_min = 200.0f;
    routing.m_replace_range_max = 800.0f;
    routing.m_has_replace_range = true;
    matrix.add_routing(routing);

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    // src=0 with inverted routing → rmax
    EXPECT_FLOAT_EQ(handle.load(0), 800.0f);
}

TEST(ModulationMatrix, ReplaceRange_InvertedMidpointUnchanged) {
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
    routing.m_depth = -1.0f;
    routing.m_combine_mode = CombineMode::Replace;
    routing.m_replace_range_min = 200.0f;
    routing.m_replace_range_max = 800.0f;
    routing.m_has_replace_range = true;
    matrix.add_routing(routing);

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    // src=0.5 is symmetric — inversion shouldn't move it
    EXPECT_FLOAT_EQ(handle.load(0), 500.0f);
}

// Regression for reported bug: a pad sweep from 0→1 with an inverted, full-span
// [0, param_max] mapping previously produced rmin across the whole sweep (every
// note collapsed to the lowest). Sweep the source through {0, 0.25, 0.5, 0.75,
// 1} and assert the output traces rmax → rmin linearly.
TEST(ModulationMatrix, ReplaceRange_InvertedFullRangeSweep) {
    thl::State state;
    state.create(
        "note",
        thl::ParameterDefinition::make_float("Note", thl::Range::linear(0.0f, 127.0f), 60.0f)
            .modulatable(true));
    ModulationMatrix matrix(state);

    ConstSource src;
    src.m_value = 0.0f;

    matrix.add_source("src", &src);
    auto handle = matrix.get_smart_handle<float>("note");

    ModulationRouting routing;
    routing.m_source_id = "src";
    routing.m_target_id = "note";
    routing.m_depth = -1.0f;
    routing.m_combine_mode = CombineMode::Replace;
    routing.m_replace_range_min = 0.0f;
    routing.m_replace_range_max = 127.0f;
    routing.m_has_replace_range = true;
    matrix.add_routing(routing);

    matrix.prepare(k_sample_rate, k_block_size);

    const std::array<std::pair<float, float>, 5> cases{
        {{0.0f, 127.0f}, {0.25f, 95.25f}, {0.5f, 63.5f}, {0.75f, 31.75f}, {1.0f, 0.0f}}};
    for (const auto& [input, expected] : cases) {
        src.m_value = input;
        matrix.process(k_block_size);
        EXPECT_FLOAT_EQ(handle.load(0), expected) << "input=" << input;
    }
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
    EXPECT_TRUE(has_mono_additive(matrix.get_target("p1")));
    EXPECT_TRUE(has_mono_additive(matrix.get_target("p2")));

    // Remove only the first routing by ID.
    matrix.remove_routing(id1);
    matrix.process(k_block_size);

    EXPECT_FALSE(has_mono_additive(matrix.get_target("p1")));
    EXPECT_TRUE(has_mono_additive(matrix.get_target("p2")));

    // Remove the second.
    matrix.remove_routing(id2);
    matrix.process(k_block_size);
    EXPECT_FALSE(has_mono_additive(matrix.get_target("p2")));
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
    EXPECT_FLOAT_EQ(mono_of(target)->m_additive_buffer[0], 0.5f);

    // Update by ID.
    EXPECT_TRUE(matrix.update_routing_depth(id, 0.25f));
    matrix.process(k_block_size);
    EXPECT_FLOAT_EQ(mono_of(target)->m_additive_buffer[0], 0.25f);

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
    EXPECT_TRUE(has_mono_additive(target));
    EXPECT_FLOAT_EQ(mono_of(target)->m_additive_buffer[0], 1.0f);

    // Begin gesture — modulation should be suppressed (buffer stays zero).
    state.set_gesture_from_root("freq", true);
    matrix.process(k_block_size);
    for (size_t i = 0; i < k_block_size; ++i) {
        EXPECT_FLOAT_EQ(mono_of(target)->m_additive_buffer[i], 0.0f) << "Sample " << i;
    }

    // End gesture — modulation should resume.
    state.set_gesture_from_root("freq", false);
    matrix.process(k_block_size);
    EXPECT_TRUE(has_mono_additive(target));
    EXPECT_FLOAT_EQ(mono_of(target)->m_additive_buffer[0], 1.0f);
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
    EXPECT_TRUE(has_mono_additive(target));
    EXPECT_FLOAT_EQ(mono_of(target)->m_additive_buffer[0], 1.0f);
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
    EXPECT_FLOAT_EQ(mono_of(target)->m_additive_buffer[0], 0.75f);

    // During gesture — only src2 contributes.
    state.set_gesture_from_root("freq", true);
    matrix.process(k_block_size);
    EXPECT_FLOAT_EQ(mono_of(target)->m_additive_buffer[0], 0.25f);
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
    EXPECT_FALSE(mono_of(target)->m_change_points.empty());

    // During gesture — no change points.
    state.set_gesture_from_root("freq", true);
    matrix.process(k_block_size);
    EXPECT_TRUE(mono_of(target)->m_change_points.empty());
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

// ─────────────────────────────────────────────────────────────────────────
// pre_process_block drain pass
// ─────────────────────────────────────────────────────────────────────────

// Tracking source that records when pre_process_block / process are called
// relative to a shared monotonic counter. Used to verify invocation ordering.
class TrackingSource : public ModulationSource {
public:
    int* m_shared_counter = nullptr;
    int m_pre_tick = -1;
    int m_process_tick = -1;
    int m_pre_call_count = 0;
    float m_value = 1.0f;

    TrackingSource() : ModulationSource(true, 0, true) {}

    void prepare(double /*sr*/, size_t spb) override { resize_buffers(spb); }

    void pre_process_block() override {
        ++m_pre_call_count;
        if (m_shared_counter != nullptr) { m_pre_tick = (*m_shared_counter)++; }
    }

    void process(size_t num_samples, size_t offset = 0) override {
        if (m_shared_counter != nullptr && m_process_tick == -1) {
            m_process_tick = (*m_shared_counter)++;
        }
        for (size_t i = offset; i < offset + num_samples; ++i) { m_output_buffer[i] = m_value; }
        if (num_samples > 0) { record_change_point(static_cast<uint32_t>(offset)); }
    }
};

TEST(ModulationMatrix, PreProcessBlockCalledOncePerBlockPerSource) {
    thl::State state;
    state.create("freq", modulatable_float(0.0f));
    ModulationMatrix matrix(state);

    TrackingSource a;
    TrackingSource b;
    matrix.add_source("a", &a);
    matrix.add_source("b", &b);
    matrix.get_smart_handle<float>("freq");
    matrix.add_routing({"a", "freq", 1.0f});
    // NOTE: "b" has no routing — it must still be drained.

    matrix.prepare(k_sample_rate, k_block_size);

    for (int block = 0; block < 5; ++block) { matrix.process(k_block_size); }

    EXPECT_EQ(a.m_pre_call_count, 5);
    EXPECT_EQ(b.m_pre_call_count, 5);
}

TEST(ModulationMatrix, PreProcessBlockRunsBeforeAnyProcess) {
    thl::State state;
    state.create("a_out", modulatable_float(0.0f));
    state.create("b_out", modulatable_float(0.0f));
    ModulationMatrix matrix(state);

    int counter = 0;
    TrackingSource a;
    TrackingSource b;
    a.m_shared_counter = &counter;
    b.m_shared_counter = &counter;

    matrix.add_source("a", &a);
    matrix.add_source("b", &b);
    matrix.get_smart_handle<float>("a_out");
    matrix.get_smart_handle<float>("b_out");
    matrix.add_routing({"a", "a_out", 1.0f});
    matrix.add_routing({"b", "b_out", 1.0f});

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    // Both pre ticks must occur before both process ticks.
    ASSERT_GE(a.m_pre_tick, 0);
    ASSERT_GE(b.m_pre_tick, 0);
    ASSERT_GE(a.m_process_tick, 0);
    ASSERT_GE(b.m_process_tick, 0);
    EXPECT_LT(a.m_pre_tick, a.m_process_tick);
    EXPECT_LT(a.m_pre_tick, b.m_process_tick);
    EXPECT_LT(b.m_pre_tick, a.m_process_tick);
    EXPECT_LT(b.m_pre_tick, b.m_process_tick);
}

TEST(ModulationMatrix, PreProcessBlockCalledOnUnroutedSource) {
    // A source with no outgoing routings still participates in the drain pass.
    // Regression: m_all_sources must include every registered source, not
    // just those reachable from a routing.
    thl::State state;
    state.create("freq", modulatable_float(0.0f));
    ModulationMatrix matrix(state);

    TrackingSource lonely;
    matrix.add_source("lonely", &lonely);
    // No routing for "lonely".

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    EXPECT_EQ(lonely.m_pre_call_count, 1);
}

TEST(ModulationMatrix, RemoveSourceStopsDrain) {
    thl::State state;
    state.create("freq", modulatable_float(0.0f));
    ModulationMatrix matrix(state);

    TrackingSource s;
    matrix.add_source("s", &s);

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);
    EXPECT_EQ(s.m_pre_call_count, 1);

    matrix.remove_source("s");
    matrix.process(k_block_size);
    // Count unchanged — source is no longer drained.
    EXPECT_EQ(s.m_pre_call_count, 1);
}

// =============================================================================
// Multi-Replace priority composition tests (MonoToMono)
// =============================================================================

// Source with configurable mid-block active-mask transition. Active for samples
// [0, switch_at), inactive for samples [switch_at, block) — or vice versa
// depending on m_initial_active.
class ActiveMaskSource : public ModulationSource {
public:
    float m_value = 0.0f;
    bool m_initial_active = true;
    uint32_t m_switch_at = 0;  // 0 = constant (no switch)

    ActiveMaskSource() : ModulationSource(true, 0, false) {}
    void prepare(double /*sr*/, size_t spb) override { resize_buffers(spb); }
    void process(size_t num_samples, size_t offset = 0) override {
        for (size_t i = offset; i < offset + num_samples; ++i) {
            m_output_buffer[i] = m_value;
            const bool active = (m_switch_at == 0)
                                    ? m_initial_active
                                    : (i < m_switch_at ? m_initial_active : !m_initial_active);
            if (active) { set_output_active(static_cast<uint32_t>(i)); }
        }
        if (num_samples > 0) {
            record_change_point(static_cast<uint32_t>(offset));
            if (m_switch_at > offset && m_switch_at < offset + num_samples) {
                record_change_point(m_switch_at);
            }
        }
    }
};

TEST(ModulationMatrix, MultiReplace_MidBlockActiveTransition_WinnerFlips) {
    // Three-priority setup. p_mid active across whole block; p_hi active only
    // in second half; p_lo inactive. Winner should be p_mid for the first
    // half, p_hi for the second half. Demonstrates correct sub-segment
    // composition across the union of change points.
    thl::State state;
    state.create("cutoff", modulatable_float(0.5f));
    ModulationMatrix matrix(state);

    ActiveMaskSource p_hi;
    p_hi.m_value = 0.9f;
    p_hi.m_initial_active = false;
    p_hi.m_switch_at = k_block_size / 2;

    ActiveMaskSource p_mid;
    p_mid.m_value = 0.5f;
    p_mid.m_initial_active = true;
    p_mid.m_switch_at = 0;  // always active

    ActiveMaskSource p_lo;
    p_lo.m_value = 0.1f;
    p_lo.m_initial_active = false;
    p_lo.m_switch_at = 0;  // always inactive

    matrix.add_source("hi", &p_hi);
    matrix.add_source("mid", &p_mid);
    matrix.add_source("lo", &p_lo);

    auto handle = matrix.get_smart_handle<float>("cutoff");

    ModulationRouting r_hi{"hi", "cutoff", 1.0f, 0, DepthMode::Absolute, CombineMode::Replace};
    r_hi.m_priority = 10;
    ModulationRouting r_mid{"mid", "cutoff", 1.0f, 0, DepthMode::Absolute, CombineMode::Replace};
    r_mid.m_priority = 5;
    ModulationRouting r_lo{"lo", "cutoff", 1.0f, 0, DepthMode::Absolute, CombineMode::Replace};
    r_lo.m_priority = 1;

    matrix.add_routing(r_hi);
    matrix.add_routing(r_mid);
    matrix.add_routing(r_lo);

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    // First half: only mid active → 0.5.
    EXPECT_FLOAT_EQ(handle.load(0), 0.5f);
    EXPECT_FLOAT_EQ(handle.load(k_block_size / 4), 0.5f);

    // Second half: both mid and hi active → hi (higher priority) wins.
    EXPECT_FLOAT_EQ(handle.load(k_block_size / 2), 0.9f);
    EXPECT_FLOAT_EQ(handle.load(k_block_size - 1), 0.9f);
}

TEST(ModulationMatrix, MultiReplaceHold_HigherPriorityBlocksLower) {
    // Higher-priority ReplaceHold is active briefly, then its source goes
    // silent. Per the plan, ReplaceHold's "effective active mask" is sticky —
    // after the initial trigger, the higher-priority hold continues to win
    // over a lower-priority Replace.
    thl::State state;
    state.create("cutoff", modulatable_float(0.5f));
    ModulationMatrix matrix(state);

    // hi is active only in the first quarter of the block with value 0.9.
    ActiveMaskSource hi;
    hi.m_value = 0.9f;
    hi.m_initial_active = true;
    hi.m_switch_at = k_block_size / 4;

    // lo is active across the whole block with value 0.2.
    ActiveMaskSource lo;
    lo.m_value = 0.2f;
    lo.m_initial_active = true;
    lo.m_switch_at = 0;

    matrix.add_source("hi", &hi);
    matrix.add_source("lo", &lo);

    auto handle = matrix.get_smart_handle<float>("cutoff");

    ModulationRouting r_hi{"hi", "cutoff", 1.0f, 0, DepthMode::Absolute, CombineMode::ReplaceHold};
    r_hi.m_priority = 10;
    ModulationRouting r_lo{"lo", "cutoff", 1.0f, 0, DepthMode::Absolute, CombineMode::Replace};
    r_lo.m_priority = 1;

    matrix.add_routing(r_hi);
    matrix.add_routing(r_lo);

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    // First quarter: hi active → 0.9 wins.
    EXPECT_FLOAT_EQ(handle.load(0), 0.9f);
    EXPECT_FLOAT_EQ(handle.load(k_block_size / 8), 0.9f);

    // After hi goes silent: its ReplaceHold keeps writing 0.9, which beats lo.
    EXPECT_FLOAT_EQ(handle.load(k_block_size / 4), 0.9f);
    EXPECT_FLOAT_EQ(handle.load(k_block_size - 1), 0.9f);
}

TEST(ModulationMatrix, MultiReplace_HigherPriorityAdditiveOnly_StillComposesWithReplace) {
    // Additive routings are never deferred. Mixing Additive + multi-Replace on
    // the same target still works — Additive is applied in-schedule, the two
    // Replace routings defer and compose in the post-pass.
    thl::State state;
    state.create("cutoff", modulatable_float(0.0f));
    ModulationMatrix matrix(state);

    ConstSource add_src;
    add_src.m_value = 0.1f;
    ActiveMaskSource rep_hi;
    rep_hi.m_value = 0.7f;
    rep_hi.m_initial_active = true;
    rep_hi.m_switch_at = 0;
    ActiveMaskSource rep_lo;
    rep_lo.m_value = 0.3f;
    rep_lo.m_initial_active = true;
    rep_lo.m_switch_at = 0;

    matrix.add_source("add", &add_src);
    matrix.add_source("rep_hi", &rep_hi);
    matrix.add_source("rep_lo", &rep_lo);

    auto handle = matrix.get_smart_handle<float>("cutoff");

    matrix.add_routing({"add", "cutoff", 1.0f, 0, DepthMode::Absolute, CombineMode::Additive});
    ModulationRouting r_hi{"rep_hi", "cutoff", 1.0f, 0, DepthMode::Absolute, CombineMode::Replace};
    r_hi.m_priority = 10;
    ModulationRouting r_lo{"rep_lo", "cutoff", 1.0f, 0, DepthMode::Absolute, CombineMode::Replace};
    r_lo.m_priority = 1;
    matrix.add_routing(r_hi);
    matrix.add_routing(r_lo);

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    // rep_hi wins the Replace write (0.7); Additive contributes 0.1 on top.
    EXPECT_FLOAT_EQ(handle.load(0), 0.7f + 0.1f);
}

TEST(ModulationSource, DefaultPreProcessBlockIsNoOp) {
    // A source that only overrides process()/process_voice() should still
    // compose cleanly with the matrix — the default pre_process_block is a no-op.
    thl::State state;
    state.create("freq", modulatable_float(0.0f));
    ModulationMatrix matrix(state);

    ConstSource src;  // Does not override pre_process_block
    matrix.add_source("src", &src);
    matrix.get_smart_handle<float>("freq");
    matrix.add_routing({"src", "freq", 1.0f});

    matrix.prepare(k_sample_rate, k_block_size);
    // Must not crash. Base class default is a no-op.
    matrix.process(k_block_size);

    const auto* target = matrix.get_target("freq");
    ASSERT_NE(target, nullptr);
    // Modulation still occurred normally.
    EXPECT_TRUE(has_mono_additive(target));
}
