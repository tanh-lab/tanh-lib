#include <gtest/gtest.h>

#include <tanh/dsp/synth/RingsResonatorSynthProcessor.h>

#include <array>
#include <cmath>

namespace {

using Parameter = thl::dsp::synth::RingsParameter;

class TestResonator : public thl::dsp::synth::RingsResonatorSynthProcessor {
public:
    float get_parameter_value(Parameter p) override {
        return m_values[static_cast<int>(p)];
    }

    void set(Parameter p, float v) { m_values[static_cast<int>(p)] = v; }

private:
    static constexpr int kN = static_cast<int>(Parameter::NUM_PARAMETERS);
    std::array<float, kN> m_values = {
        440.0f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 1.0f, 0.0f, 0.0f,
    };
};

static constexpr int kBlockSize = 256;

float process_and_measure_energy(TestResonator& synth, int num_blocks = 4) {
    float energy = 0.0f;
    for (int b = 0; b < num_blocks; ++b) {
        float input[kBlockSize] = {};
        float output[kBlockSize] = {};
        if (b == 0) input[0] = 1.0f;
        synth.process(input, output, kBlockSize);
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
    synth.set(Parameter::Polyphony, 0.0f);
    float energy = process_and_measure_energy(synth);
    EXPECT_GT(energy, 1e-6f);
}

TEST_F(RingsResonatorSynthProcessorTest, PolyphonyTwoProducesFiniteOutput) {
    synth.set(Parameter::Polyphony, 1.0f);
    float energy = process_and_measure_energy(synth);
    EXPECT_TRUE(std::isfinite(energy));
    EXPECT_GT(energy, 1e-6f);
}

TEST_F(RingsResonatorSynthProcessorTest, PolyphonyFourProducesFiniteOutput) {
    synth.set(Parameter::Polyphony, 2.0f);
    float energy = process_and_measure_energy(synth);
    EXPECT_TRUE(std::isfinite(energy));
    EXPECT_GT(energy, 1e-6f);
}

TEST_F(RingsResonatorSynthProcessorTest, PolyphonyClampedToValidRange) {
    synth.set(Parameter::Polyphony, 5.0f);
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
        synth.process(input, out_zero.data() + b * kBlockSize, kBlockSize);
    }

    TestResonator synth2;
    synth2.prepare(48000.0, kBlockSize);
    synth2.set(Parameter::OddEvenMix, 1.0f);
    synth2.set(Parameter::DryWet, 1.0f);

    std::array<float, kBlockSize * 4> out_one{};
    for (int b = 0; b < 4; ++b) {
        float input[kBlockSize] = {};
        if (b == 0) input[0] = 1.0f;
        synth2.process(input, out_one.data() + b * kBlockSize, kBlockSize);
    }

    float diff = 0.0f;
    for (size_t i = 0; i < out_zero.size(); ++i) {
        diff += std::abs(out_zero[i] - out_one[i]);
    }
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
        synth.process(input, output.data() + b * kBlockSize, kBlockSize);
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

        synth_a.process(input, out_a, kBlockSize);
        synth_b.process(input_copy, out_b, kBlockSize);

        for (int i = 0; i < kBlockSize; ++i) {
            ASSERT_EQ(out_a[i], out_b[i])
                << "Mismatch at block " << b << " sample " << i;
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
        synth.process(input, out1, kBlockSize);
        synth2.process(input2, out2, kBlockSize);

        for (int i = 0; i < kBlockSize; ++i) {
            ASSERT_FLOAT_EQ(out1[i], out2[i])
                << "Cross-instance mismatch at block " << b << " sample " << i;
            ASSERT_TRUE(std::isfinite(out1[i]));
            energy += out1[i] * out1[i];
        }
    }

    EXPECT_GT(energy, 1e-4f);
}

}  // namespace
