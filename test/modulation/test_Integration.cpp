#include <gtest/gtest.h>

#include "TestHelpers.h"

#include <string_view>
#include <vector>

#include <tanh/dsp/audio/AudioBufferView.h>
#include <tanh/dsp/utils/Limiter.h>
#include <tanh/modulation/ModulationMatrix.h>
#include <tanh/modulation/SmartHandle.h>
#include <tanh/state/State.h>

using namespace thl::modulation;

namespace LimiterID {
constexpr std::string_view k_attack = "limiter.attack";
constexpr std::string_view k_release = "limiter.release";
constexpr std::string_view k_threshold = "limiter.threshold";
constexpr float k_attack_default = 0.f;
constexpr float k_release_default = 0.0f;
constexpr float k_threshold_default = -10.0f;
}  // namespace LimiterID

class TestLimiter : public thl::dsp::utils::LimiterImpl {
public:
    explicit TestLimiter(thl::modulation::ModulationMatrix& mmatrix) : m_mmatrix(mmatrix) {
        m_smart_handles.resize(Parameter::NumParameters);
        m_smart_handles[Parameter::Attack] = mmatrix.get_smart_handle<float>(LimiterID::k_attack);
        m_smart_handles[Parameter::Release] = mmatrix.get_smart_handle<float>(LimiterID::k_release);
        m_smart_handles[Parameter::Threshold] =
            mmatrix.get_smart_handle<float>(LimiterID::k_threshold);
    }

    void prepare(const double& sample_rate,
                 const size_t& samples_per_block,
                 const size_t& num_channels) override {
        m_change_points.reserve(samples_per_block);
        thl::dsp::utils::LimiterImpl::prepare(sample_rate, samples_per_block, num_channels);
    }

private:
    float get_parameter_float(Parameter p, uint32_t modulation_offset) override {
        return m_smart_handles[p].load(modulation_offset);
    }
    std::span<const uint32_t> get_change_points() override {
        thl::modulation::collect_change_points(
            std::span<const thl::modulation::SmartHandle<float>>(m_smart_handles),
            m_change_points);
        return m_change_points;
    }

    thl::modulation::ModulationMatrix& m_mmatrix;
    std::vector<thl::modulation::SmartHandle<float>> m_smart_handles;
    std::vector<uint32_t> m_change_points;
};

TEST(Integration, LFOModulatesLimiterThreshold) {
    // Setup
    thl::State state;
    state.create(LimiterID::k_attack, modulatable_float(LimiterID::k_attack_default));
    state.create(LimiterID::k_release, modulatable_float(LimiterID::k_release_default));
    state.create(LimiterID::k_threshold, modulatable_float(LimiterID::k_threshold_default));
    ModulationMatrix mmatrix(state);
    TestLFOSource lfo1;
    lfo1.m_frequency = 1000.0f;  // 10 Hz LFO
    lfo1.m_waveform = LFOWaveform::Sine;
    lfo1.m_decimation = 50;
    TestLFOSource lfo2;
    lfo2.m_frequency = 500.0f;
    lfo2.m_waveform = LFOWaveform::Square;
    lfo2.m_decimation = 64;

    TestLimiter test_limiter(mmatrix);

    mmatrix.add_source("lfo1", &lfo1);
    mmatrix.add_routing({"lfo1", LimiterID::k_threshold, 3.0f});  // +/-3 dB modulation
    mmatrix.add_source("lfo2", &lfo2);
    mmatrix.add_routing({"lfo2", LimiterID::k_threshold, 9.0f});

    mmatrix.prepare(k_sample_rate, k_block_size);
    test_limiter.prepare(k_sample_rate, k_block_size, 1);

    // Process one block
    mmatrix.process(k_block_size);

    const auto* target = mmatrix.get_target(LimiterID::k_threshold);
    ASSERT_NE(target, nullptr);

    // Create audio buffer
    std::vector<float> audio(k_block_size, 0.8f);
    thl::dsp::audio::AudioBufferView view(audio.data(), k_block_size);

    // Process with modulation change points
    test_limiter.process_modulated(view);

    // Verify that the audio was actually processed (not silent)
    bool has_nonzero = false;
    for (size_t i = 0; i < k_block_size; ++i) {
        if (audio[i] != 0.0f) {
            has_nonzero = true;
            break;
        }
    }
    EXPECT_TRUE(has_nonzero);
}

// ── Constant-output source for depth mode tests ──────────────────────────────

class ConstantSource : public ModulationSource {
public:
    float m_value = 1.0f;

    ConstantSource() : ModulationSource(true, 0, true) {}

    void prepare(double /*sample_rate*/, size_t samples_per_block) override {
        resize_buffers(samples_per_block);
    }

    void process(size_t num_samples, size_t offset = 0) override {
        for (size_t i = offset; i < offset + num_samples; ++i) { m_output_buffer[i] = m_value; }
        if (num_samples > 0) { record_change_point(static_cast<uint32_t>(offset)); }
    }
};

// ── Normalized Depth Mode Tests ──────────────────────────────────────────────

TEST(Integration, NormalizedDepthLinear) {
    thl::State state;
    state.create("param",
                 thl::ParameterDefinition::make_float("P", thl::Range::linear(0.0f, 100.0f), 50.0f)
                     .modulatable(true));
    ModulationMatrix matrix(state);

    ConstantSource src;
    src.m_value = 1.0f;

    matrix.add_source("src", &src);
    matrix.get_smart_handle<float>("param");
    // Normalized depth 0.5 on linear range [0,100] → abs depth = 0.5 * 100 = 50
    matrix.add_routing({"src", "param", 0.5f, 0, DepthMode::Normalized});

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    const auto* target = matrix.get_target("param");
    ASSERT_NE(target, nullptr);
    for (size_t i = 0; i < k_block_size; ++i) {
        EXPECT_FLOAT_EQ(target->m_additive_buffer[i], 50.0f) << "Mismatch at sample " << i;
    }
}

TEST(Integration, AbsoluteDepthMode) {
    thl::State state;
    state.create("param",
                 thl::ParameterDefinition::make_float("P", thl::Range::linear(0.0f, 100.0f), 50.0f)
                     .modulatable(true));
    ModulationMatrix matrix(state);

    ConstantSource src;
    src.m_value = 1.0f;

    matrix.add_source("src", &src);
    matrix.get_smart_handle<float>("param");
    // Absolute depth = 10.0 → buffer = source * depth = 1.0 * 10.0 = 10.0
    matrix.add_routing({"src", "param", 10.0f, 0, DepthMode::Absolute});

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    const auto* target = matrix.get_target("param");
    ASSERT_NE(target, nullptr);
    for (size_t i = 0; i < k_block_size; ++i) {
        EXPECT_FLOAT_EQ(target->m_additive_buffer[i], 10.0f) << "Mismatch at sample " << i;
    }
}

TEST(Integration, NormalizedDepthSkewed) {
    // Power law range: depth operates in normalized [0,1] space, then converts
    // back through the curve. Result differs from linear pre-computed path.
    thl::State state;
    state.create("freq",
                 thl::ParameterDefinition::make_float("Freq",
                                                      thl::Range::power_law(20.0f, 20000.0f, 3.0f),
                                                      440.0f)
                     .modulatable(true));
    ModulationMatrix matrix(state);

    ConstantSource src;
    src.m_value = 0.5f;

    matrix.add_source("src", &src);
    matrix.get_smart_handle<float>("freq");
    matrix.add_routing({"src", "freq", 0.3f, 0, DepthMode::Normalized});

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    const auto* target = matrix.get_target("freq");
    ASSERT_NE(target, nullptr);

    // With normalized buffer, the modulation buffer stores normalized deltas:
    // src * depth = 0.5 * 0.3 = 0.15
    const float expected_norm_delta = 0.5f * 0.3f;
    for (size_t i = 0; i < k_block_size; ++i) {
        EXPECT_FLOAT_EQ(target->m_additive_buffer[i], expected_norm_delta)
            << "Mismatch at sample " << i;
    }

    // SmartHandle::load() applies the curve conversion at read time
    auto handle = matrix.get_smart_handle<float>("freq");
    const float base = 440.0f;
    const auto& range = handle.range();
    const float base_norm = range.to_normalized(base);
    const float result_norm = std::clamp(base_norm + expected_norm_delta, 0.0f, 1.0f);
    const float expected_value = range.from_normalized(result_norm);

    EXPECT_NEAR(handle.load(0), expected_value, 0.01f);

    // Verify result differs from what a linear range would produce
    const float linear_value = base + 0.5f * 0.3f * (20000.0f - 20.0f);
    EXPECT_NE(handle.load(0), linear_value);
}

TEST(Integration, NormalizedDepthPeriodicWraps) {
    // Use a power_law periodic range to trigger the non-linear path with wrapping.
    auto range = thl::Range::power_law(0.0f, 360.0f, 2.0f);
    range.m_periodic = true;

    thl::State state;
    state.create("phase",
                 thl::ParameterDefinition::make_float("Phase", range, 350.0f).modulatable(true));
    ModulationMatrix matrix(state);

    ConstantSource src;
    src.m_value = 1.0f;

    matrix.add_source("src", &src);
    matrix.get_smart_handle<float>("phase");
    // Large enough normalized depth to push base_norm + src * depth past 1.0
    matrix.add_routing({"src", "phase", 0.3f, 0, DepthMode::Normalized});

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    const auto* target = matrix.get_target("phase");
    ASSERT_NE(target, nullptr);

    // With normalized buffer, the buffer stores normalized deltas:
    // src * depth = 1.0 * 0.3 = 0.3
    const float expected_norm_delta = 1.0f * 0.3f;
    for (size_t i = 0; i < k_block_size; ++i) {
        EXPECT_FLOAT_EQ(target->m_additive_buffer[i], expected_norm_delta)
            << "Mismatch at sample " << i;
    }

    // SmartHandle::load() applies wrapping in normalized space at read time
    auto handle = matrix.get_smart_handle<float>("phase");
    const float base = 350.0f;
    const float base_norm = range.to_normalized(base);
    float result_norm = base_norm + expected_norm_delta;
    // Wrap: fmod then shift positive
    result_norm = std::fmod(result_norm, 1.0f);
    if (result_norm < 0.0f) { result_norm += 1.0f; }
    const float expected_value = range.from_normalized(result_norm);

    // The final value should be near the bottom of the range (wrapped past max)
    EXPECT_LT(expected_value, base);
    EXPECT_NEAR(handle.load(0), expected_value, 0.01f);
}

TEST(Integration, NormalizedDepthClamps) {
    // Linear range with base near max + large depth → should clamp at max
    thl::State state;
    state.create("param",
                 thl::ParameterDefinition::make_float("P", thl::Range::linear(0.0f, 100.0f), 90.0f)
                     .modulatable(true));
    ModulationMatrix matrix(state);

    ConstantSource src;
    src.m_value = 1.0f;

    matrix.add_source("src", &src);
    auto handle = matrix.get_smart_handle<float>("param");
    // Normalized depth 0.5 → abs depth 50. Base 90 + 50 = 140 > max 100.
    // For linear path, no clamping in modulation — just depth * range
    matrix.add_routing({"src", "param", 0.5f, 0, DepthMode::Normalized});

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    // Linear fast path: buffer = 50.0 regardless of base. SmartHandle reads 90+50=140.
    // Clamping is the DSP processor's responsibility for linear ranges.
    EXPECT_FLOAT_EQ(handle.load(0), 90.0f + 50.0f);
}

TEST(Integration, MixedAbsoluteAndNormalizedOnSameTarget) {
    thl::State state;
    state.create("param",
                 thl::ParameterDefinition::make_float("P", thl::Range::linear(0.0f, 100.0f), 50.0f)
                     .modulatable(true));
    ModulationMatrix matrix(state);

    ConstantSource src_abs;
    src_abs.m_value = 1.0f;
    ConstantSource src_norm;
    src_norm.m_value = 1.0f;

    matrix.add_source("abs", &src_abs);
    matrix.add_source("norm", &src_norm);
    matrix.get_smart_handle<float>("param");

    // Absolute: 1.0 * 5.0 = 5.0
    matrix.add_routing({"abs", "param", 5.0f, 0, DepthMode::Absolute});
    // Normalized: 1.0 * 0.2 * 100 = 20.0
    matrix.add_routing({"norm", "param", 0.2f, 0, DepthMode::Normalized});

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    const auto* target = matrix.get_target("param");
    ASSERT_NE(target, nullptr);
    for (size_t i = 0; i < k_block_size; ++i) {
        EXPECT_FLOAT_EQ(target->m_additive_buffer[i], 25.0f) << "Mismatch at sample " << i;
    }
}

// ── Multi-type SmartHandle Tests ─────────────────────────────────────────────

TEST(Integration, SmartHandleInt) {
    thl::State state;
    state.create("choice",
                 thl::ParameterDefinition::make_int("Choice", thl::Range::discrete(0, 3), 1)
                     .modulatable(true));
    ModulationMatrix matrix(state);

    ConstantSource src;
    src.m_value = 1.0f;

    matrix.add_source("src", &src);
    auto handle = matrix.get_smart_handle<int>("choice");
    // Absolute depth = 1.5 → modulation buffer = 1.5
    // SmartHandle<int>::load() = round(1 + 1.5) = round(2.5) = 3 (rounds to nearest)
    matrix.add_routing({"src", "choice", 1.5f, 0, DepthMode::Absolute});

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    EXPECT_EQ(handle.load(0), 3);
}

TEST(Integration, SmartHandleBool) {
    thl::State state;
    state.create("toggle", thl::ParameterDefinition::make_bool("Toggle", false).modulatable(true));
    ModulationMatrix matrix(state);

    ConstantSource src;
    src.m_value = 1.0f;

    matrix.add_source("src", &src);
    auto handle = matrix.get_smart_handle<bool>("toggle");
    // Absolute depth = 0.6 → modulation buffer = 0.6
    // SmartHandle<bool>::load() = (0.0 + 0.6) >= 0.5 → true
    matrix.add_routing({"src", "toggle", 0.6f, 0, DepthMode::Absolute});

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    EXPECT_TRUE(handle.load(0));

    // With negative source, bool should stay false
    src.m_value = -1.0f;
    matrix.process(k_block_size);
    // mod = -0.6, base 0.0 + (-0.6) = -0.6 < 0.5 → false
    EXPECT_FALSE(handle.load(0));
}

TEST(Integration, SmartHandleDouble) {
    thl::State state;
    thl::ParameterDefinition def;
    def.m_name = "DoublePrecise";
    def.m_type = thl::ParameterType::Double;
    def.m_range = thl::Range::linear(0.0f, 1.0f);
    def.m_default_value = 0.5;
    def.m_flags = thl::ParameterFlags::k_modulatable;
    state.create("dprecise", std::move(def));

    ModulationMatrix matrix(state);

    ConstantSource src;
    src.m_value = 1.0f;

    matrix.add_source("src", &src);
    auto handle = matrix.get_smart_handle<double>("dprecise");
    matrix.add_routing({"src", "dprecise", 0.25f, 0, DepthMode::Absolute});

    matrix.prepare(k_sample_rate, k_block_size);
    matrix.process(k_block_size);

    // SmartHandle<double>::load() = 0.5 + static_cast<double>(0.25) = 0.75
    EXPECT_DOUBLE_EQ(handle.load(0), 0.75);
}
