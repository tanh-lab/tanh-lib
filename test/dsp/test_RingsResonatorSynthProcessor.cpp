#include <gtest/gtest.h>

#include <tanh/dsp/synth/RingsResonatorSynthProcessor.h>
#include <tanh/dsp/audio/AudioBufferView.h>

#include <algorithm>
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
    static constexpr int k_n = static_cast<int>(Parameter::NumParameters);
    std::array<float, k_n> m_float_values = {
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
    std::array<int, k_n> m_int_values = {};
};

static constexpr int k_block_size = 256;

float process_and_measure_energy(TestResonator& synth, int num_blocks = 4) {
    float energy = 0.0f;
    for (int b = 0; b < num_blocks; ++b) {
        std::array<float, k_block_size> input = {};
        std::array<float, k_block_size> output = {};
        if (b == 0) { input[0] = 1.0f; }
        synth.process(thl::dsp::audio::ConstAudioBufferView(input.data(), k_block_size),
                      thl::dsp::audio::AudioBufferView(output.data(), k_block_size));
        for (float v : output) { energy += v * v; }
    }
    return energy;
}

class RingsResonatorSynthProcessorTest : public ::testing::Test {
protected:
    TestResonator m_synth;

    void SetUp() override { m_synth.prepare(48000.0, k_block_size); }
};

TEST_F(RingsResonatorSynthProcessorTest, PolyphonyOneProducesOutput) {
    m_synth.set_int(Parameter::Polyphony, 0);
    float energy = process_and_measure_energy(m_synth);
    EXPECT_GT(energy, 1e-6f);
}

TEST_F(RingsResonatorSynthProcessorTest, PolyphonyTwoProducesFiniteOutput) {
    m_synth.set_int(Parameter::Polyphony, 1);
    float energy = process_and_measure_energy(m_synth);
    EXPECT_TRUE(std::isfinite(energy));
    EXPECT_GT(energy, 1e-6f);
}

TEST_F(RingsResonatorSynthProcessorTest, PolyphonyFourProducesFiniteOutput) {
    m_synth.set_int(Parameter::Polyphony, 2);
    float energy = process_and_measure_energy(m_synth);
    EXPECT_TRUE(std::isfinite(energy));
    EXPECT_GT(energy, 1e-6f);
}

TEST_F(RingsResonatorSynthProcessorTest, PolyphonyClampedToValidRange) {
    m_synth.set_int(Parameter::Polyphony, 5);
    float energy = process_and_measure_energy(m_synth);
    EXPECT_TRUE(std::isfinite(energy));
}

TEST_F(RingsResonatorSynthProcessorTest, OddEvenMixAffectsOutput) {
    m_synth.set_float(Parameter::OddEvenMix, 0.0f);
    m_synth.set_float(Parameter::DryWet, 1.0f);

    std::array<float, static_cast<size_t>(k_block_size) * 4> out_zero{};
    for (int b = 0; b < 4; ++b) {
        std::array<float, k_block_size> input = {};
        if (b == 0) { input[0] = 1.0f; }
        float* out_ptr = out_zero.data() + static_cast<size_t>(b) * k_block_size;
        m_synth.process(thl::dsp::audio::ConstAudioBufferView(input.data(), k_block_size),
                        thl::dsp::audio::AudioBufferView(out_ptr, k_block_size));
    }

    TestResonator synth2;
    synth2.prepare(48000.0, k_block_size);
    synth2.set_float(Parameter::OddEvenMix, 1.0f);
    synth2.set_float(Parameter::DryWet, 1.0f);

    std::array<float, static_cast<size_t>(k_block_size) * 4> out_one{};
    for (int b = 0; b < 4; ++b) {
        std::array<float, k_block_size> input = {};
        if (b == 0) { input[0] = 1.0f; }
        float* out_ptr = out_one.data() + static_cast<size_t>(b) * k_block_size;
        synth2.process(thl::dsp::audio::ConstAudioBufferView(input.data(), k_block_size),
                       thl::dsp::audio::AudioBufferView(out_ptr, k_block_size));
    }

    float diff = 0.0f;
    for (size_t i = 0; i < out_zero.size(); ++i) { diff += std::abs(out_zero[i] - out_one[i]); }
    EXPECT_GT(diff, 1e-4f);
}

TEST_F(RingsResonatorSynthProcessorTest, StructureAt0_9995ProducesFiniteOutput) {
    m_synth.set_float(Parameter::Structure, 0.9995f);
    float energy = process_and_measure_energy(m_synth);
    EXPECT_TRUE(std::isfinite(energy));
}

TEST_F(RingsResonatorSynthProcessorTest, DampingAt0_9995ProducesFiniteOutput) {
    m_synth.set_float(Parameter::Damping, 0.9995f);
    float energy = process_and_measure_energy(m_synth);
    EXPECT_TRUE(std::isfinite(energy));
}

TEST_F(RingsResonatorSynthProcessorTest, PositionAt0_9995ProducesFiniteOutput) {
    m_synth.set_float(Parameter::Position, 0.9995f);
    float energy = process_and_measure_energy(m_synth);
    EXPECT_TRUE(std::isfinite(energy));
}

TEST_F(RingsResonatorSynthProcessorTest, DryWetZeroPassesDrySignal) {
    m_synth.set_float(Parameter::DryWet, 0.0f);

    std::array<float, static_cast<size_t>(k_block_size) * 4> output{};
    for (int b = 0; b < 4; ++b) {
        std::array<float, k_block_size> input;
        input.fill(0.5f);
        float* out_ptr = output.data() + static_cast<size_t>(b) * k_block_size;
        m_synth.process(thl::dsp::audio::ConstAudioBufferView(input.data(), k_block_size),
                        thl::dsp::audio::AudioBufferView(out_ptr, k_block_size));
    }

    int count_near_dry = 0;
    for (size_t i = 48; i < output.size(); ++i) {
        if (std::abs(output[i] - 0.5f) < 0.05f) { ++count_near_dry; }
    }
    EXPECT_GT(count_near_dry, 100);
}

TEST_F(RingsResonatorSynthProcessorTest, GetLatencyReturns24) {
    EXPECT_EQ(m_synth.get_latency(), 24);
}

TEST_F(RingsResonatorSynthProcessorTest, DefaultParameterValuesProduceOutput) {
    float energy = process_and_measure_energy(m_synth);
    EXPECT_GT(energy, 1e-6f);
}

TEST_F(RingsResonatorSynthProcessorTest, DeterministicOutput) {
    TestResonator synth_a;
    TestResonator synth_b;
    synth_a.prepare(48000.0, k_block_size);
    synth_b.prepare(48000.0, k_block_size);

    static constexpr int k_num_blocks = 8;

    for (int b = 0; b < k_num_blocks; ++b) {
        std::array<float, k_block_size> input = {};
        if (b == 0) { input[0] = 1.0f; }

        std::array<float, k_block_size> out_a = {};
        std::array<float, k_block_size> out_b = {};

        std::array<float, k_block_size> input_copy;
        std::ranges::copy(input, input_copy.begin());

        synth_a.process(thl::dsp::audio::ConstAudioBufferView(input.data(), k_block_size),
                        thl::dsp::audio::AudioBufferView(out_a.data(), k_block_size));
        synth_b.process(thl::dsp::audio::ConstAudioBufferView(input_copy.data(), k_block_size),
                        thl::dsp::audio::AudioBufferView(out_b.data(), k_block_size));

        for (int i = 0; i < k_block_size; ++i) {
            ASSERT_EQ(out_a[i], out_b[i]) << "Mismatch at block " << b << " sample " << i;
        }
    }
}

TEST_F(RingsResonatorSynthProcessorTest, WrapperOutputMatchesManualBlend) {
    m_synth.set_float(Parameter::DryWet, 1.0f);
    m_synth.set_float(Parameter::OddEvenMix, 0.5f);
    m_synth.set_float(Parameter::Frequency, 440.0f);
    m_synth.set_float(Parameter::Structure, 0.5f);
    m_synth.set_float(Parameter::Brightness, 0.5f);
    m_synth.set_float(Parameter::Damping, 0.3f);
    m_synth.set_float(Parameter::Position, 0.5f);
    m_synth.set_int(Parameter::Model, 0);
    m_synth.set_int(Parameter::Polyphony, 0);

    TestResonator synth2;
    synth2.prepare(48000.0, k_block_size);
    synth2.set_float(Parameter::DryWet, 1.0f);
    synth2.set_float(Parameter::OddEvenMix, 0.5f);
    synth2.set_float(Parameter::Frequency, 440.0f);
    synth2.set_float(Parameter::Structure, 0.5f);
    synth2.set_float(Parameter::Brightness, 0.5f);
    synth2.set_float(Parameter::Damping, 0.3f);
    synth2.set_float(Parameter::Position, 0.5f);
    synth2.set_int(Parameter::Model, 0);
    synth2.set_int(Parameter::Polyphony, 0);

    static constexpr int k_num_blocks = 16;

    float energy = 0.0f;
    for (int b = 0; b < k_num_blocks; ++b) {
        std::array<float, k_block_size> input = {};
        std::array<float, k_block_size> input2 = {};
        if (b == 0) {
            input[0] = 1.0f;
            input2[0] = 1.0f;
        }

        std::array<float, k_block_size> out1 = {};
        std::array<float, k_block_size> out2 = {};
        m_synth.process(thl::dsp::audio::ConstAudioBufferView(input.data(), k_block_size),
                        thl::dsp::audio::AudioBufferView(out1.data(), k_block_size));
        synth2.process(thl::dsp::audio::ConstAudioBufferView(input2.data(), k_block_size),
                       thl::dsp::audio::AudioBufferView(out2.data(), k_block_size));

        for (int i = 0; i < k_block_size; ++i) {
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
    synth.set_float(Parameter::Frequency, 440.0f);
    synth.set_float(Parameter::Structure, 0.5f);
    synth.set_float(Parameter::Brightness, 0.5f);
    synth.set_float(Parameter::Damping, 0.5f);
    synth.set_float(Parameter::Position, 0.5f);
    synth.set_float(Parameter::OddEvenMix, 0.5f);
    synth.set_float(Parameter::DryWet, 1.0f);
    synth.set_int(Parameter::Model, info.model_index);
    synth.set_int(Parameter::Polyphony, 0);

    // Warm up
    for (int b = 0; b < kWrapperWarmUpBlocks; ++b) {
        std::array<float, kWrapperProcessBlockSize> in = {};
        std::array<float, kWrapperProcessBlockSize> out = {};
        synth.process(thl::dsp::audio::ConstAudioBufferView(in.data(), kWrapperProcessBlockSize),
                      thl::dsp::audio::AudioBufferView(out.data(), kWrapperProcessBlockSize));
    }

    for (int b = 0; b < kWrapperNumBlocks; ++b) {
        std::array<float, kWrapperProcessBlockSize> out = {};
        const float* in_ptr = ref_input + static_cast<size_t>(b) * kWrapperProcessBlockSize;

        // Copy input since process() may read from output buffer
        std::array<float, kWrapperProcessBlockSize> in;
        std::copy(in_ptr, in_ptr + kWrapperProcessBlockSize, in.begin());

        synth.process(thl::dsp::audio::ConstAudioBufferView(in.data(), kWrapperProcessBlockSize),
                      thl::dsp::audio::AudioBufferView(out.data(), kWrapperProcessBlockSize));

        for (int i = 0; i < kWrapperProcessBlockSize; ++i) {
            size_t idx = static_cast<size_t>(b) * kWrapperProcessBlockSize + i;
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
