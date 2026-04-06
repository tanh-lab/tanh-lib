#include <gtest/gtest.h>

#include "TestHelpers.h"

#include <tanh/modulation/ModulationMatrix.h>
#include <tanh/modulation/ModulationSource.h>
#include <tanh/state/State.h>

using namespace thl::modulation;

// Test modulation source with configurable parameter keys for dependency graph.
// Outputs a fixed value (configurable via set_value) every sample.
class TestModSource : public ModulationSource {
public:
    explicit TestModSource(std::vector<std::string> keys = {}) : m_keys(std::move(keys)) {}

    void prepare(double /*sample_rate*/, size_t samples_per_block) override {
        resize_buffers(samples_per_block);
    }

    void process(size_t num_samples) override {
        m_change_points.clear();
        for (size_t i = 0; i < num_samples; ++i) {
            m_last_output = m_value;
            m_output_buffer[i] = m_last_output;
        }
        if (num_samples > 0) { m_change_points.push_back(0); }
    }

    void process_single(float* out, uint32_t sample_index) override {
        m_last_output = m_value;
        *out = m_last_output;
        m_output_buffer[sample_index] = m_last_output;
        if (sample_index == 0) { record_change_point(0); }
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
        EXPECT_FLOAT_EQ(target_a->m_modulation_buffer[i], 0.7f * 2.0f)
            << "param_a mismatch at sample " << i;
    }

    // param_b receives src1's output (0.5) * depth (3.0) = 1.5
    const auto* target_b = matrix.get_target("param_b");
    ASSERT_NE(target_b, nullptr);
    for (size_t i = 0; i < k_block_size; ++i) {
        EXPECT_FLOAT_EQ(target_b->m_modulation_buffer[i], 0.5f * 3.0f)
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
        EXPECT_FLOAT_EQ(plain->m_modulation_buffer[i], 1.0f);
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
    EXPECT_FALSE(target_a->m_change_points.empty());
    EXPECT_FALSE(target_b->m_change_points.empty());
}
