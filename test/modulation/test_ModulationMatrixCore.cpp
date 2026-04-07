#include <gtest/gtest.h>

#include "TestHelpers.h"

#include <tanh/modulation/ModulationMatrix.h>
#include <tanh/state/State.h>

using namespace thl::modulation;

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

TEST(ModulationMatrix, UnresolvedRoutingIgnored) {
    thl::State state;
    ModulationMatrix matrix(state);
    TestLFOSource lfo;
    lfo.m_frequency = 1.0f;

    matrix.add_source("lfo1", &lfo);
    // Don't add a target — routing should be silently ignored
    matrix.add_routing({"lfo1", "nonexistent", 100.0f});

    matrix.prepare(k_sample_rate, k_block_size);
    // Should not crash
    matrix.process(k_block_size);
}
