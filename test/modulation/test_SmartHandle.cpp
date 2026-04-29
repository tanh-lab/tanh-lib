#include <gtest/gtest.h>

#include "TestHelpers.h"

#include <array>

#include <tanh/modulation/ModulationMatrix.h>
#include <tanh/modulation/SmartHandle.h>
#include <tanh/state/State.h>

using namespace thl::modulation;

TEST(SmartHandle, LoadWithoutModulation) {
    SmartHandle<float> handle;
    EXPECT_FALSE(handle.is_valid());
    EXPECT_EQ(handle.change_points(), nullptr);
}

TEST(SmartHandle, GetSmartHandleFromState) {
    thl::State state;
    state.create("freq", modulatable_float(440.0f));
    ModulationMatrix matrix(state);

    auto handle = matrix.get_smart_handle<float>("freq");
    EXPECT_TRUE(handle.is_valid());
    EXPECT_FLOAT_EQ(handle.load(), 440.0f);

    // No routings → no MonoBuffers allocated → change_points() returns nullptr.
    // Buffers are created lazily on the first schedule rebuild that produces
    // routings touching this target.
    EXPECT_EQ(handle.change_points(), nullptr);
}

TEST(SmartHandle, ThrowsOnNonModulatableParameter) {
    thl::State state;
    state.create("gain",
                 thl::ParameterDefinition::make_float("Gain", thl::Range::linear(0.0f, 1.0f), 1.0f)
                     .modulatable(false));
    ModulationMatrix matrix(state);

    EXPECT_THROW(matrix.get_smart_handle<float>("gain"), std::invalid_argument);
}

TEST(SmartHandle, ModulationOffsetReadsBuffer) {
    thl::State state;
    state.create("freq", modulatable_float(440.0f));
    ModulationMatrix matrix(state);

    TestLFOSource lfo;
    lfo.m_frequency = 10.0f;
    lfo.m_waveform = LFOWaveform::Sine;

    matrix.add_source("lfo1", &lfo);
    auto handle = matrix.get_smart_handle<float>("freq");
    matrix.add_routing({"lfo1", "freq", 100.0f});

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    // SmartHandle should read base + modulation at each offset
    const auto* target = matrix.get_target("freq");
    for (size_t i = 0; i < k_block_size; ++i) {
        float expected = 440.0f + mono_of(target)->m_additive_buffer[i];
        EXPECT_FLOAT_EQ(handle.load(static_cast<uint32_t>(i)), expected)
            << "Mismatch at sample " << i;
    }
}

TEST(SmartHandle, ChangePointsPopulatedAfterProcess) {
    thl::State state;
    state.create("freq", modulatable_float(440.0f));
    ModulationMatrix matrix(state);

    TestLFOSource lfo;
    lfo.m_frequency = 10.0f;
    lfo.m_decimation = 1;

    matrix.add_source("lfo1", &lfo);
    auto handle = matrix.get_smart_handle<float>("freq");
    matrix.add_routing({"lfo1", "freq", 1.0f});

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    ASSERT_NE(handle.change_points(), nullptr);
    EXPECT_FALSE(handle.change_points()->empty());
}

TEST(SmartHandle, CollectChangePointsFromHandles) {
    thl::State state;
    state.create("a", modulatable_float(1.0f));
    state.create("b", modulatable_float(2.0f));
    ModulationMatrix matrix(state);

    TestLFOSource lfo;
    lfo.m_frequency = 10.0f;
    lfo.m_decimation = 1;

    matrix.add_source("lfo", &lfo);
    auto handle_a = matrix.get_smart_handle<float>("a");
    auto handle_b = matrix.get_smart_handle<float>("b");
    matrix.add_routing({"lfo", "a", 1.0f});
    matrix.add_routing({"lfo", "b", 1.0f});

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    // Both handles should have change points
    ASSERT_NE(handle_a.change_points(), nullptr);
    ASSERT_NE(handle_b.change_points(), nullptr);
    EXPECT_FALSE(handle_a.change_points()->empty());

    // collect_change_points from SmartHandle span
    std::array<SmartHandle<float>, 2> handles = {handle_a, handle_b};
    size_t max_buffer_size = std::max(handle_a.get_buffer_size(), handle_b.get_buffer_size());
    std::vector<uint32_t> merged_change_points;
    merged_change_points.reserve(max_buffer_size);
    collect_change_points(std::span<const SmartHandle<float>>(handles), merged_change_points);
    EXPECT_EQ(merged_change_points.size(), 512);
    EXPECT_EQ(merged_change_points[0], 0);
    EXPECT_EQ(merged_change_points[200], 200);
    EXPECT_EQ(merged_change_points[511], 511);
}

TEST(SmartHandle, UnmodulatedReadsBaseDirectly) {
    thl::State state;
    state.create("freq", modulatable_float(440.0f));
    ModulationMatrix matrix(state);

    // get_smart_handle with no routing — reads base value
    auto handle = matrix.get_smart_handle<float>("freq");

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

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

    std::vector<uint32_t> merged;
    merged.reserve(16);
    collect_change_points({std::span<const uint32_t>(list1),
                           std::span<const uint32_t>(list2),
                           std::span<const uint32_t>(list3)},
                          merged);

    std::vector<uint32_t> expected = {0, 1, 3, 5, 10, 12, 15};
    EXPECT_EQ(merged, expected);
}

TEST(CollectChangePoints, EmptyInput) {
    std::vector<uint32_t> merged;
    merged.reserve(16);
    collect_change_points({}, merged);
    EXPECT_TRUE(merged.empty());
}

// SmartHandle metadata accessor tests

TEST(SmartHandle, DefAccessor) {
    thl::State state;
    state.create("freq",
                 thl::ParameterDefinition::make_float("Frequency",
                                                      thl::Range::linear(20.0f, 20000.0f),
                                                      440.0f,
                                                      1)
                     .automatable(false)
                     .modulatable(true));

    ModulationMatrix matrix(state);
    matrix.prepare(k_sample_rate, k_block_size);
    auto handle = matrix.get_smart_handle<float>("freq");

    EXPECT_EQ("Frequency", handle.def().m_name);
    EXPECT_EQ(thl::ParameterType::Float, handle.def().m_type);
    EXPECT_EQ(1u, handle.def().m_decimal_places);
}

TEST(SmartHandle, RangeAccessor) {
    thl::State state;
    state.create("gain", modulatable_float(0.5f));

    ModulationMatrix matrix(state);
    matrix.prepare(k_sample_rate, k_block_size);
    auto handle = matrix.get_smart_handle<float>("gain");

    EXPECT_FLOAT_EQ(0.0f, handle.range().m_min);
    EXPECT_FLOAT_EQ(1.0f, handle.range().m_max);
}

TEST(SmartHandle, KeyAccessor) {
    thl::State state;
    state.create("synth.cutoff", modulatable_float(1000.0f));

    ModulationMatrix matrix(state);
    matrix.prepare(k_sample_rate, k_block_size);
    auto handle = matrix.get_smart_handle<float>("synth.cutoff");

    EXPECT_EQ("synth.cutoff", handle.key());
}

TEST(SmartHandle, LoadNormalized) {
    thl::State state;
    state.create("pan",
                 thl::ParameterDefinition::make_float("Pan", thl::Range::linear(-1.0f, 1.0f), 0.0f)
                     .automatable(false)
                     .modulatable(true));

    ModulationMatrix matrix(state);
    matrix.prepare(k_sample_rate, k_block_size);
    auto handle = matrix.get_smart_handle<float>("pan");

    // pan = 0.0 in [-1, 1] -> normalized = 0.5
    EXPECT_FLOAT_EQ(0.5f, handle.load_normalized());

    // Change to max -> normalized = 1.0
    state.set("pan", 1.0f);
    EXPECT_FLOAT_EQ(1.0f, handle.load_normalized());

    // Change to min -> normalized = 0.0
    state.set("pan", -1.0f);
    EXPECT_FLOAT_EQ(0.0f, handle.load_normalized());
}

// =============================================================================
// change_point_flags / change_point_flags_voice / change_points_voice
// accessors — introduced so mid-block readers (e.g. relay sources) can see
// change points already stamped by Tarjan-earlier sources inside the same
// block. The built list variants are only populated after all sources have
// run, so they would be stale mid-block.
// =============================================================================

TEST(SmartHandle, ChangePointFlags_SetAtMonoChangePointOffsets) {
    thl::State state;
    state.create("freq", modulatable_float(0.5f));
    ModulationMatrix matrix(state);

    TestLFOSource lfo;
    lfo.m_frequency = 10.0f;
    lfo.m_decimation = 1;  // change point every sample
    matrix.add_source("lfo", &lfo);
    auto handle = matrix.get_smart_handle<float>("freq");
    matrix.add_routing({"lfo", "freq", 1.0f});

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    const uint8_t* flags = handle.change_point_flags();
    ASSERT_NE(flags, nullptr);

    // decimation=1 → every sample is a change point
    for (size_t i = 0; i < k_block_size; ++i) {
        EXPECT_EQ(flags[i], 1) << "Expected flag set at sample " << i;
    }

    // And the flag bitmask should agree with the post-build list.
    const auto* list = handle.change_points();
    ASSERT_NE(list, nullptr);
    EXPECT_EQ(list->size(), k_block_size);
}

TEST(SmartHandle, ChangePointFlags_NullptrWhenNoMonoBuffer) {
    thl::State state;
    ModulationMatrix matrix(state);
    const auto voice_scope = matrix.register_scope("voice", 2);
    state.create("freq", modulatable_float(0.5f, voice_scope));

    // Poly-only routing → no MonoBuffers allocated for this target.
    PolyTestSource poly(voice_scope);
    poly.m_voice_values = {0.1f, 0.2f};
    matrix.add_source("poly", &poly);
    auto handle = matrix.get_smart_handle<float>("freq");
    matrix.add_routing({"poly", "freq", 1.0f});

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    EXPECT_EQ(handle.change_point_flags(), nullptr);
}

TEST(SmartHandle, ChangePointFlags_NullptrOnUnattachedHandle) {
    SmartHandle<float> handle;  // default-constructed, no target
    EXPECT_EQ(handle.change_point_flags(), nullptr);
}

TEST(SmartHandle, ChangePointFlagsVoice_SetAtVoiceChangePointOffsets) {
    thl::State state;
    ModulationMatrix matrix(state);
    const auto voice_scope = matrix.register_scope("voice", 3);
    state.create("freq", modulatable_float(0.5f, voice_scope));

    // PolyTestSource records exactly one voice change point per voice, at
    // offset 0 (see TestHelpers.h). So flags[0] must be set for every voice
    // and all other samples must be zero.
    PolyTestSource poly(voice_scope);
    poly.m_voice_values = {0.1f, 0.2f, 0.3f};
    matrix.add_source("poly", &poly);
    auto handle = matrix.get_smart_handle<float>("freq");
    matrix.add_routing({"poly", "freq", 1.0f});

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    for (uint32_t v = 0; v < 3; ++v) {
        const uint8_t* flags = handle.change_point_flags_voice(v);
        ASSERT_NE(flags, nullptr) << "voice " << v;
        EXPECT_EQ(flags[0], 1) << "voice " << v << " expected change-point flag at sample 0";
        for (size_t i = 1; i < k_block_size; ++i) {
            EXPECT_EQ(flags[i], 0) << "voice " << v << " unexpected flag at sample " << i;
        }
    }
}

TEST(SmartHandle, ChangePointFlagsVoice_AgreesWithBuiltList) {
    thl::State state;
    ModulationMatrix matrix(state);
    const auto voice_scope = matrix.register_scope("voice", 2);
    state.create("freq", modulatable_float(0.5f, voice_scope));

    PolyTestSource poly(voice_scope);
    poly.m_voice_values = {0.4f, 0.6f};
    matrix.add_source("poly", &poly);
    auto handle = matrix.get_smart_handle<float>("freq");
    matrix.add_routing({"poly", "freq", 1.0f});

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    // After build_change_points runs at end-of-block, the list must equal
    // the set-bit positions of the live flag bitmask.
    for (uint32_t v = 0; v < 2; ++v) {
        const uint8_t* flags = handle.change_point_flags_voice(v);
        const auto* list = handle.change_points_voice(v);
        ASSERT_NE(flags, nullptr);
        ASSERT_NE(list, nullptr);

        std::vector<uint32_t> from_flags;
        for (size_t i = 0; i < k_block_size; ++i) {
            if (flags[i]) { from_flags.push_back(static_cast<uint32_t>(i)); }
        }
        EXPECT_EQ(from_flags, *list) << "voice " << v;
    }
}

TEST(SmartHandle, ChangePointFlagsVoice_NullptrWhenNoVoiceBuffer) {
    thl::State state;
    state.create("freq", modulatable_float(0.5f));
    ModulationMatrix matrix(state);

    // Mono-only routing → no VoiceBuffers allocated.
    TestLFOSource lfo;
    lfo.m_frequency = 10.0f;
    matrix.add_source("lfo", &lfo);
    auto handle = matrix.get_smart_handle<float>("freq");
    matrix.add_routing({"lfo", "freq", 1.0f});

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    EXPECT_EQ(handle.change_point_flags_voice(0), nullptr);
    EXPECT_EQ(handle.change_points_voice(0), nullptr);
}

TEST(SmartHandle, ChangePointFlagsVoice_NullptrForOutOfRangeVoice) {
    thl::State state;
    ModulationMatrix matrix(state);
    const auto voice_scope = matrix.register_scope("voice", 2);
    state.create("freq", modulatable_float(0.5f, voice_scope));

    PolyTestSource poly(voice_scope);
    poly.m_voice_values = {0.1f, 0.2f};
    matrix.add_source("poly", &poly);
    auto handle = matrix.get_smart_handle<float>("freq");
    matrix.add_routing({"poly", "freq", 1.0f});

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    // In-range accessor succeeds; out-of-range returns nullptr rather than
    // indexing into the per-voice storage.
    EXPECT_NE(handle.change_point_flags_voice(0), nullptr);
    EXPECT_NE(handle.change_point_flags_voice(1), nullptr);
    EXPECT_EQ(handle.change_point_flags_voice(2), nullptr);
    EXPECT_EQ(handle.change_points_voice(2), nullptr);
}

TEST(SmartHandle, ChangePointFlagsVoice_NullptrOnUnattachedHandle) {
    SmartHandle<float> handle;
    EXPECT_EQ(handle.change_point_flags_voice(0), nullptr);
    EXPECT_EQ(handle.change_points_voice(0), nullptr);
}

// Regression guard for the post-scope-refactor allocation rule:
// a per-voice-scope target reached only by a Global source allocates
// MonoBuffers (no VoiceBuffers). DSP voice processors call
// collect_change_points(handles, buf, voice=v); that helper must fall through
// to the mono change-points for every voice index so sub-block splits still
// land on the Global source's transitions.
TEST(SmartHandle, CollectChangePoints_FallsThroughMonoForVoiceScope) {
    thl::State state;
    ModulationMatrix matrix(state);
    const auto voice_scope = matrix.register_scope("voice", 4);
    state.create("freq", modulatable_float(0.5f, voice_scope));

    // LFO is Global — routes to a voice-scope target, triggers GlobalToScoped.
    // Under the new rule this allocates MonoBuffers only (no poly sources
    // routed here). The LFO records mono change points at offset 0 per block.
    TestLFOSource lfo;
    lfo.m_frequency = 10.0f;
    lfo.m_decimation = 1;
    matrix.add_source("lfo", &lfo);
    auto handle = matrix.get_smart_handle<float>("freq");
    matrix.add_routing({"lfo", "freq", 1.0f});

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    // VoiceBuffers must NOT be allocated (no same-scope routing).
    const auto* target = matrix.get_target("freq");
    ASSERT_NE(target, nullptr);
    EXPECT_EQ(voice_of(target), nullptr);
    ASSERT_NE(mono_of(target), nullptr);

    // Helper must return the mono change points regardless of voice_index.
    std::array<SmartHandle<float>, 1> handles{handle};
    std::vector<uint32_t> buf;
    buf.reserve(k_block_size);
    for (uint32_t v = 0; v < 4; ++v) {
        collect_change_points(std::span<const SmartHandle<float>>(handles), buf, v);
        EXPECT_FALSE(buf.empty()) << "voice " << v << ": expected mono CPs to fall through";
    }
}

TEST(SmartHandle, DisplayFormattingViaSmartHandle) {
    thl::State state;
    state.create("freq",
                 thl::ParameterDefinition::make_float("Frequency",
                                                      thl::Range::linear(20.0f, 20000.0f),
                                                      440.0f,
                                                      1)
                     .automatable(false)
                     .modulatable(true));

    ModulationMatrix matrix(state);
    matrix.prepare(k_sample_rate, k_block_size);
    auto handle = matrix.get_smart_handle<float>("freq");

    ASSERT_TRUE(handle.def().m_value_to_text);
    EXPECT_EQ("440.0", handle.def().m_value_to_text(handle.load()));
}
