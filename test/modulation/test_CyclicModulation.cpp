#include <gtest/gtest.h>
#include <tanh/modulation/ModulationMatrix.h>
#include <tanh/modulation/ModulationSource.h>
#include <tanh/modulation/SmartHandle.h>
#include <tanh/state/State.h>

#include "TestHelpers.h"

using namespace thl::modulation;

// Test modulation source with configurable parameter keys for dependency graph.
// Outputs a fixed value (configurable via set_value) every sample.
class TestModSource : public ModulationSource {
public:
    explicit TestModSource(std::vector<std::string> keys = {})
        : ModulationSource(thl::modulation::k_global_scope, true), m_keys(std::move(keys)) {}

    void prepare(double /*sample_rate*/, size_t samples_per_block, uint32_t voice_count) override {
        resize_buffers(samples_per_block, voice_count);
    }

    void process(size_t num_samples, size_t offset = 0) override {
        for (size_t i = offset; i < offset + num_samples; ++i) {
            m_last_output = m_value;
            m_output_buffer[i] = m_last_output;
        }
        if (num_samples > 0) { record_change_point(static_cast<uint32_t>(offset)); }
    }

    std::vector<std::string> parameter_keys() const override { return m_keys; }

    void set_value(float v) { m_value = v; }

private:
    std::vector<std::string> m_keys;
    float m_value = 1.0f;
};

TEST(CyclicModulation, IndependentSourcesAreBulkSteps) {
    thl::State state;
    ModulationMatrix matrix(state);
    TestModSource src1;
    TestModSource src2;

    matrix.add_source("src1", &src1);
    matrix.add_source("src2", &src2);
    matrix.add_routing({"src1", "target1", 1.0f});
    matrix.add_routing({"src2", "target2", 1.0f});

    matrix.prepare(k_sample_rate, k_block_size);

    const auto& schedule = matrix.get_schedule();
    ASSERT_EQ(schedule.size(), 2u);
    for (auto& step : schedule) { EXPECT_TRUE(std::holds_alternative<BulkStep>(step)); }
}

TEST(CyclicModulation, CrossRoutingCreatesCyclicStep) {
    thl::State state;
    state.create("param_a", modulatable_float(0.0f));
    state.create("param_b", modulatable_float(0.0f));
    ModulationMatrix matrix(state);

    // src1 owns "param_a", src2 owns "param_b"
    TestModSource src1({"param_a"});
    TestModSource src2({"param_b"});

    matrix.add_source("src1", &src1);
    matrix.add_source("src2", &src2);

    // src2 routes to param_a (owned by src1) -> src1 depends on src2
    // src1 routes to param_b (owned by src2) -> src2 depends on src1
    // This creates a cycle: src1 <-> src2
    matrix.add_routing({"src2", "param_a", 1.0f});
    matrix.add_routing({"src1", "param_b", 1.0f});

    matrix.prepare(k_sample_rate, k_block_size);

    const auto& schedule = matrix.get_schedule();
    ASSERT_EQ(schedule.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<CyclicStep>(schedule[0]));

    const auto& cyclic = std::get<CyclicStep>(schedule[0]);
    EXPECT_EQ(cyclic.m_sources.size(), 2u);
}

TEST(CyclicModulation, SelfEdgeCreatesCyclicStep) {
    thl::State state;
    state.create("self_param", modulatable_float(0.0f));
    ModulationMatrix matrix(state);

    // src1 owns "self_param" and routes to it -> self-edge
    TestModSource src1({"self_param"});

    matrix.add_source("src1", &src1);
    matrix.add_routing({"src1", "self_param", 1.0f});

    matrix.prepare(k_sample_rate, k_block_size);

    const auto& schedule = matrix.get_schedule();
    ASSERT_EQ(schedule.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<CyclicStep>(schedule[0]));

    const auto& cyclic = std::get<CyclicStep>(schedule[0]);
    EXPECT_EQ(cyclic.m_sources.size(), 1u);
}

TEST(CyclicModulation, CyclicProcessingFillsModulationBuffer) {
    thl::State state;
    state.create("param_a", modulatable_float(0.0f));
    state.create("param_b", modulatable_float(0.0f));
    ModulationMatrix matrix(state);

    TestModSource src1({"param_a"});
    TestModSource src2({"param_b"});
    src1.set_value(0.5f);
    src2.set_value(0.7f);

    matrix.add_source("src1", &src1);
    matrix.add_source("src2", &src2);
    matrix.get_smart_handle<float>("param_a");
    matrix.get_smart_handle<float>("param_b");

    // Cross-routing creates a cycle
    matrix.add_routing({"src2", "param_a", 2.0f});
    matrix.add_routing({"src1", "param_b", 3.0f});

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    // param_a receives src2's output (0.7) * depth (2.0) = 1.4
    const auto* target_a = matrix.get_target("param_a");
    ASSERT_NE(target_a, nullptr);
    for (size_t i = 0; i < k_block_size; ++i) {
        EXPECT_FLOAT_EQ(mono_of(target_a)->m_additive_buffer[i], 0.7f * 2.0f)
            << "param_a mismatch at sample " << i;
    }

    // param_b receives src1's output (0.5) * depth (3.0) = 1.5
    const auto* target_b = matrix.get_target("param_b");
    ASSERT_NE(target_b, nullptr);
    for (size_t i = 0; i < k_block_size; ++i) {
        EXPECT_FLOAT_EQ(mono_of(target_b)->m_additive_buffer[i], 0.5f * 3.0f)
            << "param_b mismatch at sample " << i;
    }
}

TEST(CyclicModulation, MixedBulkAndCyclicSchedule) {
    thl::State state;
    state.create("plain_target", modulatable_float(0.0f));
    state.create("cyc_param_a", modulatable_float(0.0f));
    state.create("cyc_param_b", modulatable_float(0.0f));
    ModulationMatrix matrix(state);

    // Independent source (no parameter keys, routes to a plain target)
    TestModSource independent;
    independent.set_value(1.0f);

    // Cyclic pair
    TestModSource cyclic1({"cyc_param_a"});
    TestModSource cyclic2({"cyc_param_b"});
    cyclic1.set_value(0.3f);
    cyclic2.set_value(0.6f);

    matrix.add_source("independent", &independent);
    matrix.add_source("cyclic1", &cyclic1);
    matrix.add_source("cyclic2", &cyclic2);
    matrix.get_smart_handle<float>("plain_target");
    matrix.get_smart_handle<float>("cyc_param_a");
    matrix.get_smart_handle<float>("cyc_param_b");

    // Independent routes to plain target
    matrix.add_routing({"independent", "plain_target", 1.0f});
    // Cyclic cross-routing
    matrix.add_routing({"cyclic2", "cyc_param_a", 1.0f});
    matrix.add_routing({"cyclic1", "cyc_param_b", 1.0f});

    matrix.prepare(k_sample_rate, k_block_size);

    // Schedule should have 2 steps: one BulkStep + one CyclicStep
    const auto& schedule = matrix.get_schedule();
    size_t bulk_count = 0;
    size_t cyclic_count = 0;
    for (auto& step : schedule) {
        if (std::holds_alternative<BulkStep>(step)) { ++bulk_count; }
        if (std::holds_alternative<CyclicStep>(step)) { ++cyclic_count; }
    }
    EXPECT_EQ(bulk_count, 1u);
    EXPECT_EQ(cyclic_count, 1u);

    // Process and verify the independent target gets correct output
    matrix.process(k_block_size);

    const auto* plain = matrix.get_target("plain_target");
    ASSERT_NE(plain, nullptr);
    for (size_t i = 0; i < k_block_size; ++i) {
        EXPECT_FLOAT_EQ(mono_of(plain)->m_additive_buffer[i], 1.0f);
    }
}

TEST(CyclicModulation, DependencyChainIsTopologicallyOrdered) {
    thl::State state;
    ModulationMatrix matrix(state);

    // Chain: src_a -> param_b -> src_b -> param_c -> src_c (no cycle)
    // src_b depends on src_a, src_c depends on src_b
    TestModSource src_a;
    TestModSource src_b({"param_b"});
    TestModSource src_c({"param_c"});
    src_a.set_value(1.0f);
    src_b.set_value(2.0f);
    src_c.set_value(3.0f);

    matrix.add_source("src_a", &src_a);
    matrix.add_source("src_b", &src_b);
    matrix.add_source("src_c", &src_c);

    // src_a -> param_b (owned by src_b): src_b depends on src_a
    matrix.add_routing({"src_a", "param_b", 1.0f});
    // src_b -> param_c (owned by src_c): src_c depends on src_b
    matrix.add_routing({"src_b", "param_c", 1.0f});

    matrix.prepare(k_sample_rate, k_block_size);

    const auto& schedule = matrix.get_schedule();
    ASSERT_EQ(schedule.size(), 3u);

    // All should be BulkStep (no cycles)
    for (auto& step : schedule) { ASSERT_TRUE(std::holds_alternative<BulkStep>(step)); }

    // Topological order: src_a before src_b before src_c
    EXPECT_EQ(std::get<BulkStep>(schedule[0]).m_source, static_cast<ModulationSource*>(&src_a));
    EXPECT_EQ(std::get<BulkStep>(schedule[1]).m_source, static_cast<ModulationSource*>(&src_b));
    EXPECT_EQ(std::get<BulkStep>(schedule[2]).m_source, static_cast<ModulationSource*>(&src_c));
}

TEST(CyclicModulation, CyclicSourcesRecordChangePoints) {
    thl::State state;
    state.create("param_a", modulatable_float(0.0f));
    state.create("param_b", modulatable_float(0.0f));
    ModulationMatrix matrix(state);

    TestModSource src1({"param_a"});
    TestModSource src2({"param_b"});

    matrix.add_source("src1", &src1);
    matrix.add_source("src2", &src2);
    matrix.get_smart_handle<float>("param_a");
    matrix.get_smart_handle<float>("param_b");

    matrix.add_routing({"src2", "param_a", 1.0f});
    matrix.add_routing({"src1", "param_b", 1.0f});

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    // Targets should have change points propagated from sources
    const auto* target_a = matrix.get_target("param_a");
    const auto* target_b = matrix.get_target("param_b");
    ASSERT_NE(target_a, nullptr);
    ASSERT_NE(target_b, nullptr);
    EXPECT_FALSE(mono_of(target_a)->m_change_points.empty());
    EXPECT_FALSE(mono_of(target_b)->m_change_points.empty());
}

// Simulates an input-driven source that freezes its per-block value in
// pre_process_block — mirrors the XYTouchSource pattern. The snapshot must
// remain stable across cyclic iterations: if process() is invoked multiple
// times per block inside a CyclicStep, the "queue" must not be redrained.
class SnapshotSource : public ModulationSource {
public:
    float m_next_value = 0.0f;  // Written "from the UI thread" between blocks.
    int m_drain_count = 0;
    float m_snapshot = 0.0f;

    explicit SnapshotSource(std::vector<std::string> keys = {})
        : ModulationSource(thl::modulation::k_global_scope, true), m_keys(std::move(keys)) {}

    void prepare(double /*sr*/, size_t spb, uint32_t voice_count) override {
        resize_buffers(spb, voice_count);
    }

    void pre_process_block() override {
        ++m_drain_count;
        m_snapshot = m_next_value;  // One-shot drain of the "queue".
    }

    void process(size_t num_samples, size_t offset = 0) override {
        for (size_t i = offset; i < offset + num_samples; ++i) { m_output_buffer[i] = m_snapshot; }
        m_last_output = m_snapshot;
        if (num_samples > 0) { record_change_point(static_cast<uint32_t>(offset)); }
    }

    std::vector<std::string> parameter_keys() const override { return m_keys; }

private:
    std::vector<std::string> m_keys;
};

TEST(CyclicModulation, PreProcessBlockSnapshotStableAcrossCyclicIterations) {
    // Two sources in a cycle, one of which drains its "queue" in
    // pre_process_block. CyclicStep may iterate process() multiple times;
    // the queue-composing source must not re-drain per iteration.
    thl::State state;
    state.create("param_a", modulatable_float(0.0f));
    state.create("param_b", modulatable_float(0.0f));
    ModulationMatrix matrix(state);

    SnapshotSource src1({"param_a"});
    TestModSource src2({"param_b"});
    src1.m_next_value = 0.5f;
    src2.set_value(0.3f);

    matrix.add_source("src1", &src1);
    matrix.add_source("src2", &src2);
    matrix.get_smart_handle<float>("param_a");
    matrix.get_smart_handle<float>("param_b");

    // Cross-routing: src1 <-> src2 form a cycle.
    matrix.add_routing({"src2", "param_a", 1.0f});
    matrix.add_routing({"src1", "param_b", 1.0f});

    matrix.prepare(k_sample_rate, k_block_size);

    matrix.process(k_block_size);
    EXPECT_EQ(src1.m_drain_count, 1);
    // Snapshot reflects the pre-block value.
    EXPECT_FLOAT_EQ(src1.m_snapshot, 0.5f);

    // Between blocks, the "UI thread" writes a new value.
    src1.m_next_value = 0.9f;

    matrix.process(k_block_size);
    EXPECT_EQ(src1.m_drain_count, 2);
    EXPECT_FLOAT_EQ(src1.m_snapshot, 0.9f);
    // Block 2's output reflects the snapshot, not mid-iteration rewrites.
    for (size_t i = 0; i < k_block_size; ++i) {
        EXPECT_FLOAT_EQ(src1.get_output_buffer()[i], 0.9f);
    }
}

// A modulator that owns a single parameter, reads it via SmartHandle during its
// own process(), and emits the sampled value as its output. Used to detect
// whether upstream Replace routings are visible at the moment this source runs
// — i.e. whether the schedule put the replaces before this source's process().
class ParamReadingSource : public ModulationSource {
public:
    explicit ParamReadingSource(std::string key)
        : ModulationSource(thl::modulation::k_global_scope, true), m_key(std::move(key)) {}

    void prepare(double /*sr*/, size_t spb, uint32_t voice_count) override {
        resize_buffers(spb, voice_count);
    }

    void process(size_t num_samples, size_t offset = 0) override {
        for (size_t i = offset; i < offset + num_samples; ++i) {
            m_output_buffer[i] = m_handle.load(static_cast<uint32_t>(i));
        }
        if (num_samples > 0) { record_change_point(static_cast<uint32_t>(offset)); }
    }

    std::vector<std::string> parameter_keys() const override { return {m_key}; }

    void set_handle(SmartHandle<float> handle) { m_handle = std::move(handle); }

private:
    std::string m_key;
    SmartHandle<float> m_handle;
};

// Bug repro: when two sources Replace a parameter that belongs to a downstream
// modulator, the deferred multi-replace composition runs *after* the schedule
// loop, so the modulator reads the unmodulated base value rather than the
// replaced value. Confirms (a) the schedule still orders the replace sources
// before the modulator and (b) the modulator's output reflects the replaced
// value at process() time.
TEST(CyclicModulation, MultiReplaceOnModulatorParamVisibleDuringProcess) {
    thl::State state;
    state.create("mod_param", modulatable_float(0.0f));
    state.create("sink", modulatable_float(0.0f));
    ModulationMatrix matrix(state);

    TestModSource replace_lo;
    replace_lo.set_value(0.25f);
    TestModSource replace_hi;
    replace_hi.set_value(0.75f);
    ParamReadingSource modulator("mod_param");

    matrix.add_source("replace_lo", &replace_lo);
    matrix.add_source("replace_hi", &replace_hi);
    matrix.add_source("modulator", &modulator);

    auto mod_param_handle = matrix.get_smart_handle<float>("mod_param");
    matrix.get_smart_handle<float>("sink");
    modulator.set_handle(mod_param_handle);

    ModulationRouting r_lo("replace_lo", "mod_param", 1.0f);
    r_lo.m_combine_mode = CombineMode::Replace;
    r_lo.m_replace_priority = 1;
    matrix.add_routing(r_lo);

    ModulationRouting r_hi("replace_hi", "mod_param", 1.0f);
    r_hi.m_combine_mode = CombineMode::Replace;
    r_hi.m_replace_priority = 2;
    matrix.add_routing(r_hi);

    matrix.add_routing({"modulator", "sink", 1.0f});

    matrix.prepare(k_sample_rate, k_block_size);

    // Schedule sanity: replace_lo and replace_hi must be ordered before the
    // modulator (regardless of whether they are merged into one cyclic step
    // or three bulk steps).
    const auto schedule = matrix.get_schedule();
    auto position_of = [&](ModulationSource* s) -> int {
        for (size_t i = 0; i < schedule.size(); ++i) {
            if (auto* bulk = std::get_if<BulkStep>(&schedule[i])) {
                if (bulk->m_source == s) { return static_cast<int>(i); }
            } else if (auto* cyc = std::get_if<CyclicStep>(&schedule[i])) {
                for (auto* m : cyc->m_sources) {
                    if (m == s) { return static_cast<int>(i); }
                }
            }
        }
        return -1;
    };
    const int pos_lo = position_of(&replace_lo);
    const int pos_hi = position_of(&replace_hi);
    const int pos_mod = position_of(&modulator);
    ASSERT_GE(pos_lo, 0);
    ASSERT_GE(pos_hi, 0);
    ASSERT_GE(pos_mod, 0);
    EXPECT_LT(pos_lo, pos_mod) << "replace_lo must run before modulator";
    EXPECT_LT(pos_hi, pos_mod) << "replace_hi must run before modulator";

    matrix.process(k_block_size);

    // After the block, mod_param's replace buffer must reflect the higher-
    // priority value (post-deferred composition). This already works today.
    EXPECT_FLOAT_EQ(mod_param_handle.load(0), 0.75f);

    // The bug: modulator.process() ran *between* replace_lo/replace_hi and the
    // deferred composition pass. With the deferred pass running after the
    // schedule loop, modulator saw the unmodulated base (0.0) and emitted that
    // into its output — which is what arrives at the sink target.
    const auto* sink_target = matrix.get_target("sink");
    ASSERT_NE(sink_target, nullptr);
    const auto* sink_mono = mono_of(sink_target);
    ASSERT_NE(sink_mono, nullptr);
    for (size_t i = 0; i < k_block_size; ++i) {
        EXPECT_FLOAT_EQ(sink_mono->m_additive_buffer[i], 0.75f)
            << "modulator failed to observe replaced mod_param at sample " << i << " (saw "
            << sink_mono->m_additive_buffer[i] << ")";
    }
}
