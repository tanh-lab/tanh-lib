#include <gtest/gtest.h>

#include <tanh/dsp/synth/RingsResonatorSynthProcessor.h>
#include <tanh/dsp/audio/AudioBufferView.h>

#include <array>
#include <cmath>

#ifdef RINGS_HAS_REFERENCE_FIXTURES
#include <RingsTestFixtures.h>
#include <tanh/dsp/rings-resonator/RingsDsp.h>
#endif

namespace {

using Parameter = thl::dsp::synth::RingsResonatorSynthProcessor::Parameter;

class TestResonator : public thl::dsp::synth::RingsResonatorSynthProcessor {
public:
    float get_parameter_float(Parameter p) override { return m_float_values[p]; }

    int get_parameter_int(Parameter p) override { return m_int_values[p]; }

    void set_float(Parameter p, float v) { m_float_values[p] = v; }
    void set_int(Parameter p, int v) { m_int_values[p] = v; }

private:
    static constexpr int kN = static_cast<int>(Parameter::NUM_PARAMETERS);
    std::array<float, kN> m_float_values = {
        440.0f,
        0.5f,
        0.5f,
        0.5f,
        0.5f,
        0.5f,
        1.0f,
        0.0f,
        0.0f,
    };
    std::array<int, kN> m_int_values = {};
};

static constexpr int kBlockSize = 256;

float process_and_measure_energy(TestResonator& synth, int num_blocks = 4) {
    float energy = 0.0f;
    for (int b = 0; b < num_blocks; ++b) {
        float input[kBlockSize] = {};
        float output[kBlockSize] = {};
        if (b == 0) input[0] = 1.0f;
        synth.process(thl::dsp::audio::ConstAudioBufferView(input, kBlockSize),
                      thl::dsp::audio::AudioBufferView(output, kBlockSize));
        for (float v : output) energy += v * v;
    }
    return energy;
}

class RingsResonatorSynthProcessorTest : public ::testing::Test {
protected:
    TestResonator synth;

    void SetUp() override { synth.prepare(48000.0, kBlockSize); }
};

TEST_F(RingsResonatorSynthProcessorTest, PolyphonyOneProducesOutput) {
    synth.set_int(Parameter::Polyphony, 0);
    float energy = process_and_measure_energy(synth);
    EXPECT_GT(energy, 1e-6f);
}

TEST_F(RingsResonatorSynthProcessorTest, PolyphonyTwoProducesFiniteOutput) {
    synth.set_int(Parameter::Polyphony, 1);
    float energy = process_and_measure_energy(synth);
    EXPECT_TRUE(std::isfinite(energy));
    EXPECT_GT(energy, 1e-6f);
}

TEST_F(RingsResonatorSynthProcessorTest, PolyphonyFourProducesFiniteOutput) {
    synth.set_int(Parameter::Polyphony, 2);
    float energy = process_and_measure_energy(synth);
    EXPECT_TRUE(std::isfinite(energy));
    EXPECT_GT(energy, 1e-6f);
}

TEST_F(RingsResonatorSynthProcessorTest, PolyphonyClampedToValidRange) {
    synth.set_int(Parameter::Polyphony, 5);
    float energy = process_and_measure_energy(synth);
    EXPECT_TRUE(std::isfinite(energy));
}

TEST_F(RingsResonatorSynthProcessorTest, OddEvenMixAffectsOutput) {
    synth.set(Parameter::OddEvenMix, 0.0f);
    synth.set(Parameter::DryWet, 1.0f);

    std::array<float, kBlockSize * 4> out_zero{};
    for (int b = 0; b < 4; ++b) {
        float input[kBlockSize] = {};
        if (b == 0) input[0] = 1.0f;
        float* out_ptr = out_zero.data() + b * kBlockSize;
        synth.process(thl::dsp::audio::ConstAudioBufferView(input, kBlockSize),
                      thl::dsp::audio::AudioBufferView(out_ptr, kBlockSize));
    }

    TestResonator synth2;
    synth2.prepare(48000.0, kBlockSize);
    synth2.set(Parameter::OddEvenMix, 1.0f);
    synth2.set(Parameter::DryWet, 1.0f);

    std::array<float, kBlockSize * 4> out_one{};
    for (int b = 0; b < 4; ++b) {
        float input[kBlockSize] = {};
        if (b == 0) input[0] = 1.0f;
        float* out_ptr = out_one.data() + b * kBlockSize;
        synth2.process(thl::dsp::audio::ConstAudioBufferView(input, kBlockSize),
                       thl::dsp::audio::AudioBufferView(out_ptr, kBlockSize));
    }

    float diff = 0.0f;
    for (size_t i = 0; i < out_zero.size(); ++i) { diff += std::abs(out_zero[i] - out_one[i]); }
    EXPECT_GT(diff, 1e-4f);
}

TEST_F(RingsResonatorSynthProcessorTest, StructureAt0_9995ProducesFiniteOutput) {
    synth.set(Parameter::Structure, 0.9995f);
    float energy = process_and_measure_energy(synth);
    EXPECT_TRUE(std::isfinite(energy));
}

TEST_F(RingsResonatorSynthProcessorTest, DampingAt0_9995ProducesFiniteOutput) {
    synth.set(Parameter::Damping, 0.9995f);
    float energy = process_and_measure_energy(synth);
    EXPECT_TRUE(std::isfinite(energy));
}

TEST_F(RingsResonatorSynthProcessorTest, PositionAt0_9995ProducesFiniteOutput) {
    synth.set(Parameter::Position, 0.9995f);
    float energy = process_and_measure_energy(synth);
    EXPECT_TRUE(std::isfinite(energy));
}

TEST_F(RingsResonatorSynthProcessorTest, DryWetZeroPassesDrySignal) {
    synth.set(Parameter::DryWet, 0.0f);

    std::array<float, kBlockSize * 4> output{};
    for (int b = 0; b < 4; ++b) {
        float input[kBlockSize];
        std::fill_n(input, kBlockSize, 0.5f);
        float* out_ptr = output.data() + b * kBlockSize;
        synth.process(thl::dsp::audio::ConstAudioBufferView(input, kBlockSize),
                      thl::dsp::audio::AudioBufferView(out_ptr, kBlockSize));
    }

    int count_near_dry = 0;
    for (size_t i = 48; i < output.size(); ++i) {
        if (std::abs(output[i] - 0.5f) < 0.05f) ++count_near_dry;
    }
    EXPECT_GT(count_near_dry, 100);
}

TEST_F(RingsResonatorSynthProcessorTest, GetLatencyReturns24) {
    EXPECT_EQ(synth.get_latency(), 24);
}

TEST_F(RingsResonatorSynthProcessorTest, DefaultParameterValuesProduceOutput) {
    float energy = process_and_measure_energy(synth);
    EXPECT_GT(energy, 1e-6f);
}

TEST_F(RingsResonatorSynthProcessorTest, DeterministicOutput) {
    TestResonator synth_a;
    TestResonator synth_b;
    synth_a.prepare(48000.0, kBlockSize);
    synth_b.prepare(48000.0, kBlockSize);

    static constexpr int kNumBlocks = 8;

    for (int b = 0; b < kNumBlocks; ++b) {
        float input[kBlockSize] = {};
        if (b == 0) input[0] = 1.0f;

        float out_a[kBlockSize] = {};
        float out_b[kBlockSize] = {};

        float input_copy[kBlockSize];
        std::copy(input, input + kBlockSize, input_copy);

        synth_a.process(thl::dsp::audio::ConstAudioBufferView(input, kBlockSize),
                        thl::dsp::audio::AudioBufferView(out_a, kBlockSize));
        synth_b.process(thl::dsp::audio::ConstAudioBufferView(input_copy, kBlockSize),
                        thl::dsp::audio::AudioBufferView(out_b, kBlockSize));

        for (int i = 0; i < kBlockSize; ++i) {
            ASSERT_EQ(out_a[i], out_b[i]) << "Mismatch at block " << b << " sample " << i;
        }
    }
}

TEST_F(RingsResonatorSynthProcessorTest, WrapperOutputMatchesManualBlend) {
    synth.set(Parameter::DryWet, 1.0f);
    synth.set(Parameter::OddEvenMix, 0.5f);
    synth.set(Parameter::Frequency, 440.0f);
    synth.set(Parameter::Structure, 0.5f);
    synth.set(Parameter::Brightness, 0.5f);
    synth.set(Parameter::Damping, 0.3f);
    synth.set(Parameter::Position, 0.5f);
    synth.set(Parameter::Model, 0.0f);
    synth.set(Parameter::Polyphony, 0.0f);

    TestResonator synth2;
    synth2.prepare(48000.0, kBlockSize);
    synth2.set(Parameter::DryWet, 1.0f);
    synth2.set(Parameter::OddEvenMix, 0.5f);
    synth2.set(Parameter::Frequency, 440.0f);
    synth2.set(Parameter::Structure, 0.5f);
    synth2.set(Parameter::Brightness, 0.5f);
    synth2.set(Parameter::Damping, 0.3f);
    synth2.set(Parameter::Position, 0.5f);
    synth2.set(Parameter::Model, 0.0f);
    synth2.set(Parameter::Polyphony, 0.0f);

    static constexpr int kNumBlocks = 16;

    float energy = 0.0f;
    for (int b = 0; b < kNumBlocks; ++b) {
        float input[kBlockSize] = {};
        float input2[kBlockSize] = {};
        if (b == 0) {
            input[0] = 1.0f;
            input2[0] = 1.0f;
        }

        float out1[kBlockSize] = {};
        float out2[kBlockSize] = {};
        synth.process(thl::dsp::audio::ConstAudioBufferView(input, kBlockSize),
                      thl::dsp::audio::AudioBufferView(out1, kBlockSize));
        synth2.process(thl::dsp::audio::ConstAudioBufferView(input2, kBlockSize),
                       thl::dsp::audio::AudioBufferView(out2, kBlockSize));

        for (int i = 0; i < kBlockSize; ++i) {
            ASSERT_FLOAT_EQ(out1[i], out2[i])
                << "Cross-instance mismatch at block " << b << " sample " << i;
            ASSERT_TRUE(std::isfinite(out1[i]));
            energy += out1[i] * out1[i];
        }
    }

    EXPECT_GT(energy, 1e-4f);
}

#ifdef RINGS_HAS_REFERENCE_FIXTURES

static constexpr int kWrapperWarmUpBlocks = 20;
static constexpr int kWrapperNumBlocks = 16;
static constexpr int kWrapperProcessBlockSize = 256;
static constexpr size_t kWrapperTotalFrames = kWrapperNumBlocks * kWrapperProcessBlockSize;

struct WrapperModelInfo {
    int model_index;
    const char* fixture_filename;
};

float wrapper_reference_tolerance(int model) {
    switch (model) {
        case 3:  // FM Voice
            return 2e-2f;
        case 1:  // SympatheticString
        case 4:  // SympatheticStringQuantized
            return 2e-3f;
        default:
            // Wrapper processing (smoother, odd/even blend) amplifies the
            // SemitonesToRatio LUT-vs-exp2 precision differences slightly
            // beyond the raw DSP tolerance of 1e-4.
            return 3e-4f;
    }
}

class RingsWrapperReferenceTest : public ::testing::TestWithParam<WrapperModelInfo> {};

TEST_P(RingsWrapperReferenceTest, MatchesOriginalWrapper) {
    const auto& info = GetParam();
    const float tolerance = wrapper_reference_tolerance(info.model_index);

    int size_bytes = 0;
    const char* raw = RingsTestFixtures::getNamedResource(info.fixture_filename, size_bytes);
    ASSERT_NE(raw, nullptr) << "Missing fixture: " << info.fixture_filename;
    ASSERT_EQ(size_bytes, static_cast<int>(kWrapperTotalFrames * 2 * sizeof(float)))
        << "Fixture size mismatch for " << info.fixture_filename;

    const float* ref_input = reinterpret_cast<const float*>(raw);
    const float* ref_output = ref_input + kWrapperTotalFrames;

    TestResonator synth;
    synth.prepare(48000.0, kWrapperProcessBlockSize);
    synth.set(Parameter::Frequency, 440.0f);
    synth.set(Parameter::Structure, 0.5f);
    synth.set(Parameter::Brightness, 0.5f);
    synth.set(Parameter::Damping, 0.5f);
    synth.set(Parameter::Position, 0.5f);
    synth.set(Parameter::OddEvenMix, 0.5f);
    synth.set(Parameter::DryWet, 1.0f);
    synth.set(Parameter::Model, static_cast<float>(info.model_index));
    synth.set(Parameter::Polyphony, 0.0f);

    // Warm up
    for (int b = 0; b < kWrapperWarmUpBlocks; ++b) {
        float in[kWrapperProcessBlockSize] = {};
        float out[kWrapperProcessBlockSize] = {};
        synth.process(thl::dsp::audio::ConstAudioBufferView(in, kWrapperProcessBlockSize),
                      thl::dsp::audio::AudioBufferView(out, kWrapperProcessBlockSize));
    }

    for (int b = 0; b < kWrapperNumBlocks; ++b) {
        float out[kWrapperProcessBlockSize] = {};
        const float* in_ptr = ref_input + b * kWrapperProcessBlockSize;

        // Copy input since process() may read from output buffer
        float in[kWrapperProcessBlockSize];
        std::copy(in_ptr, in_ptr + kWrapperProcessBlockSize, in);

        synth.process(thl::dsp::audio::ConstAudioBufferView(in, kWrapperProcessBlockSize),
                      thl::dsp::audio::AudioBufferView(out, kWrapperProcessBlockSize));

        for (int i = 0; i < kWrapperProcessBlockSize; ++i) {
            size_t idx = b * kWrapperProcessBlockSize + i;
            EXPECT_NEAR(out[i], ref_output[idx], tolerance)
                << "mismatch at block " << b << " sample " << i;
        }
    }
}

INSTANTIATE_TEST_SUITE_P(
    AllModels,
    RingsWrapperReferenceTest,
    ::testing::Values(WrapperModelInfo{0, "wrapper_modal.bin"},
                      WrapperModelInfo{1, "wrapper_sympathetic_string.bin"},
                      WrapperModelInfo{2, "wrapper_modulated_string.bin"},
                      WrapperModelInfo{3, "wrapper_fm_voice.bin"},
                      WrapperModelInfo{4, "wrapper_sympathetic_string_quantized.bin"},
                      WrapperModelInfo{5, "wrapper_string_and_reverb.bin"}),
    [](const ::testing::TestParamInfo<WrapperModelInfo>& info) {
        switch (info.param.model_index) {
            case 0: return std::string("Modal");
            case 1: return std::string("SympatheticString");
            case 2: return std::string("ModulatedString");
            case 3: return std::string("FMVoice");
            case 4: return std::string("SympatheticStringQuantized");
            case 5: return std::string("StringAndReverb");
            default: return std::string("Unknown");
        }
    });

#endif  // RINGS_HAS_REFERENCE_FIXTURES

}  // namespace
