#include <gtest/gtest.h>

#include <cmath>
#include <vector>
#include <string_view>

#include <tanh/dsp/audio/AudioBufferView.h>
#include <tanh/dsp/utils/Limiter.h>
#include <tanh/modulation/LFOSource.h>
#include <tanh/modulation/ModulationMatrix.h>
#include <tanh/modulation/SmartHandle.h>
#include <tanh/state/State.h>
#include <tanh/state/ParameterDefinitions.h>

using namespace thl::modulation;

static constexpr double kSampleRate = 48000.0;
static constexpr size_t kBlockSize = 512;

// Concrete LFOSourceImpl for tests — provides parameter values directly.
class TestLFOSource : public LFOSourceImpl {
public:
    float frequency = 1.0f;
    LFOWaveform waveform = LFOWaveform::Sine;
    int decimation = 1;

private:
    float get_parameter_float(Parameter p, uint32_t) override {
        switch (p) {
            case Frequency: return frequency;
            default: return 0.0f;
        }
    }
    int get_parameter_int(Parameter p, uint32_t) override {
        switch (p) {
            case Waveform: return static_cast<int>(waveform);
            case Decimation: return decimation;
            default: return 0;
        }
    }
};

// =============================================================================
// LFOSource basic tests
// =============================================================================

TEST(LFOSource, SineOutputRange) {
    TestLFOSource lfo;
    lfo.frequency = 10.0f;
    lfo.waveform = LFOWaveform::Sine;
    lfo.prepare(kSampleRate, kBlockSize);

    lfo.process(kBlockSize);

    const auto& output = lfo.get_output_buffer();
    for (size_t i = 0; i < kBlockSize; ++i) {
        EXPECT_GE(output[i], -1.0f);
        EXPECT_LE(output[i], 1.0f);
    }
}

TEST(LFOSource, TriangleOutputRange) {
    TestLFOSource lfo;
    lfo.frequency = 10.0f;
    lfo.waveform = LFOWaveform::Triangle;
    lfo.prepare(kSampleRate, kBlockSize);

    lfo.process(kBlockSize);

    const auto& output = lfo.get_output_buffer();
    for (size_t i = 0; i < kBlockSize; ++i) {
        EXPECT_GE(output[i], -1.0f);
        EXPECT_LE(output[i], 1.0f);
    }
}

TEST(LFOSource, SawOutputRange) {
    TestLFOSource lfo;
    lfo.frequency = 10.0f;
    lfo.waveform = LFOWaveform::Saw;
    lfo.prepare(kSampleRate, kBlockSize);

    lfo.process(kBlockSize);

    const auto& output = lfo.get_output_buffer();
    for (size_t i = 0; i < kBlockSize; ++i) {
        EXPECT_GE(output[i], -1.0f);
        EXPECT_LE(output[i], 1.0f);
    }
}

TEST(LFOSource, SquareOutputValues) {
    TestLFOSource lfo;
    lfo.frequency = 10.0f;
    lfo.waveform = LFOWaveform::Square;
    lfo.prepare(kSampleRate, kBlockSize);

    lfo.process(kBlockSize);

    const auto& output = lfo.get_output_buffer();
    for (size_t i = 0; i < kBlockSize; ++i) {
        EXPECT_TRUE(output[i] == 1.0f || output[i] == -1.0f);
    }
}

TEST(LFOSource, DecimationReducesChangePoints) {
    TestLFOSource lfo_fast;
    lfo_fast.frequency = 10.0f;
    lfo_fast.decimation = 1;
    lfo_fast.prepare(kSampleRate, kBlockSize);
    lfo_fast.process(kBlockSize);

    TestLFOSource lfo_slow;
    lfo_slow.frequency = 10.0f;
    lfo_slow.decimation = 16;
    lfo_slow.prepare(kSampleRate, kBlockSize);
    lfo_slow.process(kBlockSize);

    EXPECT_GT(lfo_fast.get_change_points().size(),
              lfo_slow.get_change_points().size());
}

TEST(LFOSource, ProcessSingleMatchesBulk) {
    TestLFOSource lfo_bulk;
    lfo_bulk.frequency = 5.0f;
    lfo_bulk.waveform = LFOWaveform::Sine;
    lfo_bulk.decimation = 1;
    lfo_bulk.prepare(kSampleRate, kBlockSize);
    lfo_bulk.process(kBlockSize);

    TestLFOSource lfo_single;
    lfo_single.frequency = 5.0f;
    lfo_single.waveform = LFOWaveform::Sine;
    lfo_single.decimation = 1;
    lfo_single.prepare(kSampleRate, kBlockSize);

    std::vector<float> single_output(kBlockSize);
    for (size_t i = 0; i < kBlockSize; ++i) {
        lfo_single.process_single(&single_output[i], static_cast<uint32_t>(i));
    }

    for (size_t i = 0; i < kBlockSize; ++i) {
        EXPECT_FLOAT_EQ(lfo_bulk.get_output_buffer()[i], single_output[i])
            << "Mismatch at sample " << i;
    }
}

// =============================================================================
// ModulationMatrix basic tests
// =============================================================================

TEST(ModulationMatrix, SingleSourceSingleTarget) {
    thl::State state;
    state.set("freq", 0.0f);
    ModulationMatrix matrix(state);
    TestLFOSource lfo;
    lfo.frequency = 1.0f;
    lfo.waveform = LFOWaveform::Sine;

    matrix.add_source("lfo1", &lfo);
    matrix.get_smart_handle("freq");
    matrix.add_routing({"lfo1", "freq", 100.0f});

    matrix.prepare(kSampleRate, kBlockSize);
    matrix.process(kBlockSize);

    const auto* target = matrix.get_target("freq");
    ASSERT_NE(target, nullptr);

    // Modulation buffer should have non-zero values (sine LFO * depth 100)
    bool has_nonzero = false;
    for (size_t i = 0; i < kBlockSize; ++i) {
        if (target->modulation_buffer[i] != 0.0f) {
            has_nonzero = true;
            break;
        }
    }
    EXPECT_TRUE(has_nonzero);

    // Change points should exist
    EXPECT_FALSE(target->change_points.empty());
}

TEST(ModulationMatrix, MultipleSourcesSameTarget) {
    thl::State state;
    state.set("freq", 0.0f);
    ModulationMatrix matrix(state);

    TestLFOSource lfo1;
    lfo1.frequency = 1.0f;
    lfo1.waveform = LFOWaveform::Sine;

    TestLFOSource lfo2;
    lfo2.frequency = 5.0f;
    lfo2.waveform = LFOWaveform::Triangle;

    matrix.add_source("lfo1", &lfo1);
    matrix.add_source("lfo2", &lfo2);
    matrix.get_smart_handle("freq");
    matrix.add_routing({"lfo1", "freq", 50.0f});
    matrix.add_routing({"lfo2", "freq", 25.0f});

    matrix.prepare(kSampleRate, kBlockSize);
    matrix.process(kBlockSize);

    const auto* target = matrix.get_target("freq");
    ASSERT_NE(target, nullptr);

    // The modulation buffer should be the sum of both LFO outputs * depths
    const auto& out1 = lfo1.get_output_buffer();
    const auto& out2 = lfo2.get_output_buffer();
    for (size_t i = 0; i < kBlockSize; ++i) {
        float expected = out1[i] * 50.0f + out2[i] * 25.0f;
        EXPECT_FLOAT_EQ(target->modulation_buffer[i], expected)
            << "Mismatch at sample " << i;
    }
}

TEST(ModulationMatrix, RemoveRouting) {
    thl::State state;
    state.set("freq", 0.0f);
    ModulationMatrix matrix(state);
    TestLFOSource lfo;
    lfo.frequency = 1.0f;

    matrix.add_source("lfo1", &lfo);
    matrix.get_smart_handle("freq");
    matrix.add_routing({"lfo1", "freq", 100.0f});

    matrix.prepare(kSampleRate, kBlockSize);
    matrix.process(kBlockSize);

    // Note there's modulation
    const auto* target = matrix.get_target("freq");
    ASSERT_NE(target, nullptr);
    bool has_nonzero = false;
    for (size_t i = 0; i < kBlockSize; ++i) {
        if (target->modulation_buffer[i] != 0.0f) {
            has_nonzero = true;
            break;
        }
    }
    EXPECT_TRUE(has_nonzero);

    // Remove routing and process again
    matrix.remove_routing("lfo1", "freq");
    matrix.process(kBlockSize);

    target = matrix.get_target("freq");
    ASSERT_NE(target, nullptr);
    for (size_t i = 0; i < kBlockSize; ++i) {
        EXPECT_FLOAT_EQ(target->modulation_buffer[i], 0.0f);
    }
}

TEST(ModulationMatrix, PerRoutingDecimation) {
    thl::State state;
    state.set("freq", 0.0f);
    ModulationMatrix matrix(state);
    TestLFOSource lfo;
    lfo.frequency = 10.0f;
    lfo.decimation = 1;  // Source has max resolution

    matrix.add_source("lfo1", &lfo);
    matrix.get_smart_handle("freq");

    ModulationRouting routing;
    routing.source_id = "lfo1";
    routing.target_id = "freq";
    routing.depth = 1.0f;
    routing.max_decimation = 32;
    matrix.add_routing(routing);

    matrix.prepare(kSampleRate, kBlockSize);
    matrix.process(kBlockSize);

    const auto* target = matrix.get_target("freq");
    ASSERT_NE(target, nullptr);

    // With max_decimation = 32, there should be at least
    // kBlockSize / 32 change points from the routing alone
    EXPECT_GE(target->change_points.size(), kBlockSize / 32);
}

TEST(ModulationMatrix, UnresolvedRoutingIgnored) {
    thl::State state;
    ModulationMatrix matrix(state);
    TestLFOSource lfo;
    lfo.frequency = 1.0f;

    matrix.add_source("lfo1", &lfo);
    // Don't add a target — routing should be silently ignored
    matrix.add_routing({"lfo1", "nonexistent", 100.0f});

    matrix.prepare(kSampleRate, kBlockSize);
    // Should not crash
    matrix.process(kBlockSize);
}

// =============================================================================
// Cyclic modulation tests
// =============================================================================

// Test modulation source with configurable parameter keys for dependency graph.
// Outputs a fixed value (configurable via set_value) every sample.
class TestModSource : public ModulationSource {
public:
    explicit TestModSource(std::vector<std::string> keys = {})
        : m_keys(std::move(keys)) {}

    void prepare(double /*sample_rate*/, size_t samples_per_block) override {
        resize_buffers(samples_per_block);
    }

    void process(size_t num_samples) override {
        m_change_points.clear();
        for (size_t i = 0; i < num_samples; ++i) {
            m_last_output = m_value;
            m_output_buffer[i] = m_last_output;
        }
        if (num_samples > 0) {
            m_change_points.push_back(0);
        }
    }

    void process_single(float* out, uint32_t sample_index) override {
        m_last_output = m_value;
        *out = m_last_output;
        m_output_buffer[sample_index] = m_last_output;
        if (sample_index == 0) {
            record_change_point(0);
        }
    }

    std::vector<std::string> parameter_keys() const override {
        return m_keys;
    }

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

    matrix.prepare(kSampleRate, kBlockSize);

    const auto& schedule = matrix.get_schedule();
    ASSERT_EQ(schedule.size(), 2u);
    for (auto& step : schedule) {
        EXPECT_TRUE(std::holds_alternative<BulkStep>(step));
    }
}

TEST(CyclicModulation, CrossRoutingCreatesCyclicStep) {
    thl::State state;
    ModulationMatrix matrix(state);

    // src1 owns "param_a", src2 owns "param_b"
    TestModSource src1({"param_a"});
    TestModSource src2({"param_b"});

    matrix.add_source("src1", &src1);
    matrix.add_source("src2", &src2);

    // src2 routes to param_a (owned by src1) → src1 depends on src2
    // src1 routes to param_b (owned by src2) → src2 depends on src1
    // This creates a cycle: src1 ↔ src2
    matrix.add_routing({"src2", "param_a", 1.0f});
    matrix.add_routing({"src1", "param_b", 1.0f});

    matrix.prepare(kSampleRate, kBlockSize);

    const auto& schedule = matrix.get_schedule();
    ASSERT_EQ(schedule.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<CyclicStep>(schedule[0]));

    const auto& cyclic = std::get<CyclicStep>(schedule[0]);
    EXPECT_EQ(cyclic.sources.size(), 2u);
}

TEST(CyclicModulation, SelfEdgeCreatesCyclicStep) {
    thl::State state;
    ModulationMatrix matrix(state);

    // src1 owns "self_param" and routes to it → self-edge
    TestModSource src1({"self_param"});

    matrix.add_source("src1", &src1);
    matrix.add_routing({"src1", "self_param", 1.0f});

    matrix.prepare(kSampleRate, kBlockSize);

    const auto& schedule = matrix.get_schedule();
    ASSERT_EQ(schedule.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<CyclicStep>(schedule[0]));

    const auto& cyclic = std::get<CyclicStep>(schedule[0]);
    EXPECT_EQ(cyclic.sources.size(), 1u);
}

TEST(CyclicModulation, CyclicProcessingFillsModulationBuffer) {
    thl::State state;
    state.set("param_a", 0.0f);
    state.set("param_b", 0.0f);
    ModulationMatrix matrix(state);

    TestModSource src1({"param_a"});
    TestModSource src2({"param_b"});
    src1.set_value(0.5f);
    src2.set_value(0.7f);

    matrix.add_source("src1", &src1);
    matrix.add_source("src2", &src2);
    matrix.get_smart_handle("param_a");
    matrix.get_smart_handle("param_b");

    // Cross-routing creates a cycle
    matrix.add_routing({"src2", "param_a", 2.0f});
    matrix.add_routing({"src1", "param_b", 3.0f});

    matrix.prepare(kSampleRate, kBlockSize);
    matrix.process(kBlockSize);

    // param_a receives src2's output (0.7) * depth (2.0) = 1.4
    const auto* target_a = matrix.get_target("param_a");
    ASSERT_NE(target_a, nullptr);
    for (size_t i = 0; i < kBlockSize; ++i) {
        EXPECT_FLOAT_EQ(target_a->modulation_buffer[i], 0.7f * 2.0f)
            << "param_a mismatch at sample " << i;
    }

    // param_b receives src1's output (0.5) * depth (3.0) = 1.5
    const auto* target_b = matrix.get_target("param_b");
    ASSERT_NE(target_b, nullptr);
    for (size_t i = 0; i < kBlockSize; ++i) {
        EXPECT_FLOAT_EQ(target_b->modulation_buffer[i], 0.5f * 3.0f)
            << "param_b mismatch at sample " << i;
    }
}

TEST(CyclicModulation, MixedBulkAndCyclicSchedule) {
    thl::State state;
    state.set("plain_target", 0.0f);
    state.set("cyc_param_a", 0.0f);
    state.set("cyc_param_b", 0.0f);
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
    matrix.get_smart_handle("plain_target");
    matrix.get_smart_handle("cyc_param_a");
    matrix.get_smart_handle("cyc_param_b");

    // Independent routes to plain target
    matrix.add_routing({"independent", "plain_target", 1.0f});
    // Cyclic cross-routing
    matrix.add_routing({"cyclic2", "cyc_param_a", 1.0f});
    matrix.add_routing({"cyclic1", "cyc_param_b", 1.0f});

    matrix.prepare(kSampleRate, kBlockSize);

    // Schedule should have 2 steps: one BulkStep + one CyclicStep
    const auto& schedule = matrix.get_schedule();
    size_t bulk_count = 0;
    size_t cyclic_count = 0;
    for (auto& step : schedule) {
        if (std::holds_alternative<BulkStep>(step)) ++bulk_count;
        if (std::holds_alternative<CyclicStep>(step)) ++cyclic_count;
    }
    EXPECT_EQ(bulk_count, 1u);
    EXPECT_EQ(cyclic_count, 1u);

    // Process and verify the independent target gets correct output
    matrix.process(kBlockSize);

    const auto* plain = matrix.get_target("plain_target");
    ASSERT_NE(plain, nullptr);
    for (size_t i = 0; i < kBlockSize; ++i) {
        EXPECT_FLOAT_EQ(plain->modulation_buffer[i], 1.0f);
    }
}

TEST(CyclicModulation, DependencyChainIsTopologicallyOrdered) {
    thl::State state;
    ModulationMatrix matrix(state);

    // Chain: src_a → param_b → src_b → param_c → src_c (no cycle)
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

    // src_a → param_b (owned by src_b): src_b depends on src_a
    matrix.add_routing({"src_a", "param_b", 1.0f});
    // src_b → param_c (owned by src_c): src_c depends on src_b
    matrix.add_routing({"src_b", "param_c", 1.0f});

    matrix.prepare(kSampleRate, kBlockSize);

    const auto& schedule = matrix.get_schedule();
    ASSERT_EQ(schedule.size(), 3u);

    // All should be BulkStep (no cycles)
    for (auto& step : schedule) {
        ASSERT_TRUE(std::holds_alternative<BulkStep>(step));
    }

    // Topological order: src_a before src_b before src_c
    EXPECT_EQ(std::get<BulkStep>(schedule[0]).source,
              static_cast<ModulationSource*>(&src_a));
    EXPECT_EQ(std::get<BulkStep>(schedule[1]).source,
              static_cast<ModulationSource*>(&src_b));
    EXPECT_EQ(std::get<BulkStep>(schedule[2]).source,
              static_cast<ModulationSource*>(&src_c));
}

TEST(CyclicModulation, CyclicSourcesRecordChangePoints) {
    thl::State state;
    state.set("param_a", 0.0f);
    state.set("param_b", 0.0f);
    ModulationMatrix matrix(state);

    TestModSource src1({"param_a"});
    TestModSource src2({"param_b"});

    matrix.add_source("src1", &src1);
    matrix.add_source("src2", &src2);
    matrix.get_smart_handle("param_a");
    matrix.get_smart_handle("param_b");

    matrix.add_routing({"src2", "param_a", 1.0f});
    matrix.add_routing({"src1", "param_b", 1.0f});

    matrix.prepare(kSampleRate, kBlockSize);
    matrix.process(kBlockSize);

    // Targets should have change points propagated from sources
    const auto* target_a = matrix.get_target("param_a");
    const auto* target_b = matrix.get_target("param_b");
    ASSERT_NE(target_a, nullptr);
    ASSERT_NE(target_b, nullptr);
    EXPECT_FALSE(target_a->change_points.empty());
    EXPECT_FALSE(target_b->change_points.empty());
}

// =============================================================================
// BaseProcessor process_modulated tests
// =============================================================================

// Test processor that counts how many times process() is called
// and records the buffer sizes
class CallCountingProcessor : public thl::dsp::BaseProcessor {
public:
    std::vector<size_t> m_block_sizes;

    void prepare(const double& /*sample_rate*/,
                 const size_t& samples_per_block,
                 const size_t& /*num_channels*/) override {
                    m_block_sizes.reserve(samples_per_block);
                 }

    void process(thl::dsp::audio::AudioBufferView buffer,
                 uint32_t /*modulation_offset*/ = 0) override {
        m_block_sizes.push_back(buffer.get_num_frames());
    }
};

TEST(BaseProcessor, ProcessModulatedNoChangePoints) {
    CallCountingProcessor proc;
    proc.prepare(kSampleRate, kBlockSize, 1);

    std::vector<float> data(kBlockSize, 1.0f);
    thl::dsp::audio::AudioBufferView view(data.data(), kBlockSize);

    proc.process_modulated(view, {});
    ASSERT_EQ(proc.m_block_sizes.size(), 1u);
    EXPECT_EQ(proc.m_block_sizes[0], 512u);
}

TEST(BaseProcessor, ProcessModulatedWithChangePoints) {
    CallCountingProcessor proc;
    proc.prepare(kSampleRate, kBlockSize, 1);
    
    std::vector<float> data(512, 1.0f);
    thl::dsp::audio::AudioBufferView view(data.data(), 512);

    std::vector<uint32_t> cps = {100, 300};
    proc.process_modulated(view, std::span<const uint32_t>(cps));
    
    // Should split into 3 blocks: [0,100), [100,300), [300,512)
    ASSERT_EQ(proc.m_block_sizes.size(), 3u);
    EXPECT_EQ(proc.m_block_sizes[0], 100u);
    EXPECT_EQ(proc.m_block_sizes[1], 200u);
    EXPECT_EQ(proc.m_block_sizes[2], 212u);
}

TEST(BaseProcessor, ProcessModulatedSkipsInvalidChangePoints) {
    CallCountingProcessor proc;
    proc.prepare(kSampleRate, kBlockSize, 1);

    std::vector<float> data(512, 1.0f);
    thl::dsp::audio::AudioBufferView view(data.data(), 512);

    // Include edge cases: 0 (at start, skip), 512 (at end, skip), duplicate
    std::vector<uint32_t> cps = {0, 200, 200, 512};
    proc.process_modulated(view, std::span<const uint32_t>(cps));

    // 0 is skipped (pos starts at 0, cp <= pos), 200 first time splits,
    // 200 second time skipped (cp <= pos), 512 skipped (cp >= total)
    // Result: [0,200), [200,512)
    ASSERT_EQ(proc.m_block_sizes.size(), 2u);
    EXPECT_EQ(proc.m_block_sizes[0], 200u);
    EXPECT_EQ(proc.m_block_sizes[1], 312u);
}

// =============================================================================
// SmartHandle tests
// =============================================================================

TEST(SmartHandle, LoadWithoutModulation) {
    SmartHandle handle;
    EXPECT_FALSE(handle.is_valid());
    EXPECT_EQ(handle.change_points(), nullptr);
}

TEST(SmartHandle, GetSmartHandleFromState) {
    thl::State state;
    state.set("freq", 440.0f);
    ModulationMatrix matrix(state);

    auto handle = matrix.get_smart_handle("freq");
    EXPECT_TRUE(handle.is_valid());
    EXPECT_FLOAT_EQ(handle.load(), 440.0f);

    // Target created lazily — change_points exists but is empty (no routings)
    ASSERT_NE(handle.change_points(), nullptr);
    EXPECT_TRUE(handle.change_points()->empty());
}

// TEST(SmartHandle, ThrowsOnNonexistentParameter) {
//     thl::State state;
//     ModulationMatrix matrix(state);

//     EXPECT_THROW(matrix.get_smart_handle("nonexistent"),
//     thl::StateKeyNotFoundException);
// }

TEST(SmartHandle, ThrowsOnNonModulatableParameter) {
    thl::State state;
    state.set("gain", 1.0f);
    state.set_definition_in_root("gain",
        thl::ParameterFloat("Gain", {0.0f, 1.0f}, 1.0f, 2,
                            /* automation */ true, /* modulation */ false));
    ModulationMatrix matrix(state);

    EXPECT_THROW(matrix.get_smart_handle("gain"), std::invalid_argument);
}

TEST(SmartHandle, ModulationOffsetReadsBuffer) {
    thl::State state;
    state.set("freq", 440.0f);
    ModulationMatrix matrix(state);

    TestLFOSource lfo;
    lfo.frequency = 10.0f;
    lfo.waveform = LFOWaveform::Sine;

    matrix.add_source("lfo1", &lfo);
    auto handle = matrix.get_smart_handle("freq");
    matrix.add_routing({"lfo1", "freq", 100.0f});

    matrix.prepare(kSampleRate, kBlockSize);
    matrix.process(kBlockSize);

    // SmartHandle should read base + modulation at each offset
    const auto* target = matrix.get_target("freq");
    for (size_t i = 0; i < kBlockSize; ++i) {
        float expected = 440.0f + target->modulation_buffer[i];
        EXPECT_FLOAT_EQ(handle.load(static_cast<uint32_t>(i)), expected)
            << "Mismatch at sample " << i;
    }
}

TEST(SmartHandle, ChangePointsPopulatedAfterProcess) {
    thl::State state;
    state.set("freq", 440.0f);
    ModulationMatrix matrix(state);

    TestLFOSource lfo;
    lfo.frequency = 10.0f;
    lfo.decimation = 1;

    matrix.add_source("lfo1", &lfo);
    auto handle = matrix.get_smart_handle("freq");
    matrix.add_routing({"lfo1", "freq", 1.0f});

    matrix.prepare(kSampleRate, kBlockSize);
    matrix.process(kBlockSize);

    ASSERT_NE(handle.change_points(), nullptr);
    EXPECT_FALSE(handle.change_points()->empty());
}

TEST(SmartHandle, CollectChangePointsFromHandles) {
    thl::State state;
    state.set("a", 1.0f);
    state.set("b", 2.0f);
    ModulationMatrix matrix(state);

    TestLFOSource lfo;
    lfo.frequency = 10.0f;
    lfo.decimation = 1;

    matrix.add_source("lfo", &lfo);
    auto handle_a = matrix.get_smart_handle("a");
    auto handle_b = matrix.get_smart_handle("b");
    matrix.add_routing({"lfo", "a", 1.0f});
    matrix.add_routing({"lfo", "b", 1.0f});

    matrix.prepare(kSampleRate, kBlockSize);
    matrix.process(kBlockSize);

    // Both handles should have change points
    ASSERT_NE(handle_a.change_points(), nullptr);
    ASSERT_NE(handle_b.change_points(), nullptr);
    EXPECT_FALSE(handle_a.change_points()->empty());

    // collect_change_points from SmartHandle span
    std::array<SmartHandle, 2> handles = {handle_a, handle_b};
    size_t max_buffer_size = std::max(handle_a.get_buffer_size(), handle_b.get_buffer_size());
    std::vector<uint32_t> merged_change_points;
    merged_change_points.reserve(max_buffer_size);
    collect_change_points(std::span<const SmartHandle>(handles), merged_change_points);
    EXPECT_EQ(merged_change_points.size(), 512);
    EXPECT_EQ(merged_change_points[0], 0);
    EXPECT_EQ(merged_change_points[200], 200);
    EXPECT_EQ(merged_change_points[511], 511);
}

TEST(SmartHandle, UnmodulatedReadsBaseDirectly) {
    thl::State state;
    state.set("freq", 440.0f);
    ModulationMatrix matrix(state);

    // get_smart_handle with no routing — reads base value
    auto handle = matrix.get_smart_handle("freq");

    matrix.prepare(kSampleRate, kBlockSize);
    matrix.process(kBlockSize);

    // No modulation, so all offsets return base value
    EXPECT_FLOAT_EQ(handle.load(0), 440.0f);
    EXPECT_FLOAT_EQ(handle.load(100), 440.0f);

    // Change base value in State
    state.set("freq", 880.0f);
    EXPECT_FLOAT_EQ(handle.load(0), 880.0f);
}

TEST(CollectChangePoints, MergesMultipleLists) {
    std::vector<uint32_t> list1 = {0, 5, 10};
    std::vector<uint32_t> list2 = {3, 5, 12};
    std::vector<uint32_t> list3 = {1, 10, 15};

    auto merged = collect_change_points({
        std::span<const uint32_t>(list1),
        std::span<const uint32_t>(list2),
        std::span<const uint32_t>(list3)
    });

    std::vector<uint32_t> expected = {0, 1, 3, 5, 10, 12, 15};
    EXPECT_EQ(merged, expected);
}

TEST(CollectChangePoints, EmptyInput) {
    auto merged = collect_change_points({});
    EXPECT_TRUE(merged.empty());
}

// =============================================================================
// ResolvedTarget tests
// =============================================================================

TEST(ResolvedTarget, BuildChangePointsFromFlags) {
    ResolvedTarget target;
    target.resize(100);

    target.change_point_flags[5] = true;
    target.change_point_flags[20] = true;
    target.change_point_flags[50] = true;

    target.build_change_points();

    std::vector<uint32_t> expected = {5, 20, 50};
    EXPECT_EQ(target.change_points, expected);
}

TEST(ResolvedTarget, ClearPerBlock) {
    ResolvedTarget target;
    target.resize(100);

    target.modulation_buffer[10] = 42.0f;
    target.change_point_flags[10] = true;
    target.change_points.push_back(10);

    target.clear_per_block();

    EXPECT_FLOAT_EQ(target.modulation_buffer[10], 0.0f);
    EXPECT_FALSE(target.change_point_flags[10]);
    EXPECT_TRUE(target.change_points.empty());
}

// =============================================================================
// Integration: LFO → ModulationMatrix → Limiter with process_modulated
// =============================================================================

namespace LimiterID {
    constexpr std::string_view Attack = "limiter.attack";
    constexpr std::string_view Release = "limiter.release";
    constexpr std::string_view Threshold = "limiter.threshold";
    constexpr float AttackDefault = 0.f;
    constexpr float ReleaseDefault = 0.0f;
    constexpr float ThresholdDefault = -10.0f;
}

class TestLimiter : public thl::dsp::utils::LimiterImpl {
public:
    TestLimiter(thl::modulation::ModulationMatrix& mmatrix) : m_mmatrix(mmatrix) {
        m_smart_handles.resize(Parameter::NUM_PARAMETERS);
        m_smart_handles[Parameter::Attack] = mmatrix.get_smart_handle(LimiterID::Attack);
        m_smart_handles[Parameter::Release] = mmatrix.get_smart_handle(LimiterID::Release);
        m_smart_handles[Parameter::Threshold] = mmatrix.get_smart_handle(LimiterID::Threshold);
    }

    void prepare(const double& sample_rate,
                 const size_t& samples_per_block,
                 const size_t& num_channels) override {
                    m_change_points.reserve(samples_per_block);
                    thl::dsp::utils::LimiterImpl::prepare(sample_rate, samples_per_block, num_channels);
                 }

private:
    float get_parameter_float(Parameter p,
                              uint32_t modulation_offset) override {
        return m_smart_handles[p].load(modulation_offset);
    }
    std::span<const uint32_t> get_change_points() override {
        thl::modulation::collect_change_points(m_smart_handles, m_change_points);
        return m_change_points;
    }

    thl::modulation::ModulationMatrix& m_mmatrix;
    std::vector<thl::modulation::SmartHandle> m_smart_handles;
    std::vector<uint32_t> m_change_points;
};

TEST(Integration, LFOModulatesLimiterThreshold) {
    // Setup
    thl::State state;
    state.set(LimiterID::Attack, LimiterID::AttackDefault);
    state.set(LimiterID::Release, LimiterID::ReleaseDefault);
    state.set(LimiterID::Threshold, LimiterID::ThresholdDefault);
    ModulationMatrix mmatrix(state);
    TestLFOSource lfo1;
    lfo1.frequency = 1000.0f;  // 10 Hz LFO
    lfo1.waveform = LFOWaveform::Sine;
    lfo1.decimation = 50;
    TestLFOSource lfo2;
    lfo2.frequency = 500.0f;
    lfo2.waveform = LFOWaveform::Square;
    lfo2.decimation = 64;

    TestLimiter test_limiter(mmatrix);

    mmatrix.add_source("lfo1", &lfo1);
    mmatrix.add_routing({"lfo1", LimiterID::Threshold, 3.0f});  // ±3 dB modulation
    mmatrix.add_source("lfo2", &lfo2);
    mmatrix.add_routing({"lfo2", LimiterID::Threshold, 9.0f});

    mmatrix.prepare(kSampleRate, kBlockSize);
    test_limiter.prepare(kSampleRate, kBlockSize, 1);

    // Process one block
    mmatrix.process(kBlockSize);

    const auto* target = mmatrix.get_target(LimiterID::Threshold);
    ASSERT_NE(target, nullptr);

    // Create audio buffer
    std::vector<float> audio(kBlockSize, 0.8f);
    thl::dsp::audio::AudioBufferView view(audio.data(), kBlockSize);

    // Process with modulation change points
    test_limiter.process_modulated(view);

    // Verify that the audio was actually processed (not silent)
    bool has_nonzero = false;
    for (size_t i = 0; i < kBlockSize; ++i) {
        if (audio[i] != 0.0f) {
            has_nonzero = true;
            break;
        }
    }
    EXPECT_TRUE(has_nonzero);
}
