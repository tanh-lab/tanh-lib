#include <gtest/gtest.h>
#include <tanh/core/BufferView.h>
#include <tanh/dsp/rings-resonator/RingsDsp.h>
#include <tanh/dsp/synth/RingsResonatorSynthProcessor.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

#ifdef RINGS_HAS_REFERENCE_FIXTURES
#include <RingsTestFixtures.h>
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
        synth.process(thl::core::ConstBufferView(input.data(), k_block_size),
                      thl::core::BufferView(output.data(), k_block_size));
        for (float v : output) { energy += v * v; }
    }
    return energy;
}

std::vector<float> render_with_model(int model, int num_blocks = 8) {
    TestResonator synth;
    synth.prepare(48000.0, k_block_size);
    synth.set_int(Parameter::Model, model);
    std::vector<float> rendered;
    rendered.reserve(static_cast<size_t>(num_blocks * k_block_size));
    for (int b = 0; b < num_blocks; ++b) {
        std::array<float, k_block_size> input = {};
        std::array<float, k_block_size> output = {};
        if (b == 0) { input[0] = 1.0f; }
        synth.process(thl::core::ConstBufferView(input.data(), k_block_size),
                      thl::core::BufferView(output.data(), k_block_size));
        rendered.insert(rendered.end(), output.begin(), output.end());
    }
    return rendered;
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

// Modulated reads are unclamped, so Model can arrive below 0 or at/above Last.
// It must render like the nearest valid model instead of indexing the
// per-model gain table out of bounds (cosmos #77).
TEST_F(RingsResonatorSynthProcessorTest, OutOfRangeModelIsClampedToValidRange) {
    constexpr int k_last_valid = static_cast<int>(thl::dsp::resonator::Last) - 1;
    auto const first = render_with_model(0);
    auto const last = render_with_model(k_last_valid);

    EXPECT_EQ(render_with_model(-1), first);
    EXPECT_EQ(render_with_model(-1000), first);
    EXPECT_EQ(render_with_model(k_last_valid + 1), last);
    EXPECT_EQ(render_with_model(1000), last);
}

// ---------------------------------------------------------------------------
// Model / polyphony switching (cosmos #175): a change fades the wet signal out,
// swaps at zero and fades back in, so it never clicks.

// 100-sample host blocks, so a host-block switch lands mid Rings block (24).
static constexpr int k_switch_block = 100;
static constexpr double k_switch_sr = 48000.0;
// The switch happens at block 60 (125 ms). The resonator is driven by a
// steady low sine, so every model sounds a smooth, sustained tone whose
// sample-to-sample steps are small: a click stands out against it.
static constexpr int k_switch_at = 60;
static constexpr size_t k_switch_sample = static_cast<size_t>(k_switch_at) * k_switch_block;

// Renders `num_blocks` host blocks of a fully wet resonator driven by a 110 Hz
// sine, taking Model and Polyphony per block from `schedule`.
template <typename Schedule>
std::vector<float> render_switching(int num_blocks, Schedule schedule) {
    TestResonator synth;
    synth.prepare(k_switch_sr, k_switch_block);
    synth.set_float(Parameter::Frequency, 110.0f);
    synth.set_float(Parameter::DryWet, 1.0f);
    synth.set_float(Parameter::Damping, 0.9f);
    std::vector<float> rendered;
    rendered.reserve(static_cast<size_t>(num_blocks * k_switch_block));
    for (int b = 0; b < num_blocks; ++b) {
        auto const [model, polyphony] = schedule(b);
        synth.set_int(Parameter::Model, model);
        synth.set_int(Parameter::Polyphony, polyphony);
        std::array<float, k_switch_block> input = {};
        std::array<float, k_switch_block> output = {};
        for (int i = 0; i < k_switch_block; ++i) {
            double const t = static_cast<double>(b * k_switch_block + i) / k_switch_sr;
            input[static_cast<size_t>(i)] =
                static_cast<float>(0.1 * std::sin(2.0 * 3.14159265358979 * 110.0 * t));
        }
        synth.process(thl::core::ConstBufferView(input.data(), k_switch_block),
                      thl::core::BufferView(output.data(), k_switch_block));
        rendered.insert(rendered.end(), output.begin(), output.end());
    }
    return rendered;
}

float max_step(const std::vector<float>& x, size_t begin, size_t end) {
    float m = 0.0f;
    for (size_t i = std::max<size_t>(begin, 1); i < std::min(end, x.size()); ++i) {
        m = std::max(m, std::abs(x[i] - x[i - 1]));
    }
    return m;
}

constexpr int k_last_model = static_cast<int>(thl::dsp::resonator::Last) - 1;

// Allowed step around a switch: 2x the largest step either model makes on its
// own — the 20 ms before the switch (old model) and a 20 ms window well after
// it (new model, settled). The headroom covers the new model's own attack
// while it builds up after the fade-in; an unfaded swap jumps 10-30x. Never
// below -80 dBFS, since steps that small are inaudible whatever causes them.
float allowed_step(const std::vector<float>& x) {
    constexpr float k_floor = 1e-4f;
    float const before = max_step(x, k_switch_sample - 960, k_switch_sample);
    float const after = max_step(x, x.size() - 960, x.size());
    return std::max(2.0f * std::max(before, after), k_floor);
}

// Largest step from the switch on, over the fade-out, swap and fade-in (40 ms).
float switch_step(const std::vector<float>& x) {
    return max_step(x, k_switch_sample, k_switch_sample + 1920);
}

TEST(RingsModelSwitch, SwitchMidBlockDoesNotClick) {
    for (int from = 0; from <= k_last_model; ++from) {
        for (int to = 0; to <= k_last_model; ++to) {
            if (from == to) { continue; }
            auto const out = render_switching(k_switch_at + 40, [&](int b) {
                return std::pair{b < k_switch_at ? from : to, 0};
            });
            EXPECT_LE(switch_step(out), allowed_step(out)) << "model " << from << " -> " << to;
        }
    }
}

TEST(RingsModelSwitch, SweepThroughAllModelsDoesNotClick) {
    // An LFO-like ramp through every model, one step every 12.5 ms, starting
    // at the switch point.
    auto const out = render_switching(k_switch_at + 60, [](int b) {
        int const step = b < k_switch_at ? 0 : (b - k_switch_at) / 6;
        return std::pair{std::min(step, k_last_model), 0};
    });
    // The sweep visits every model, so its ceiling is the largest settled
    // step of any of them on their own.
    float ceiling = 0.0f;
    for (int model = 0; model <= k_last_model; ++model) {
        auto const fixed =
            render_switching(k_switch_at + 60, [&](int) { return std::pair{model, 0}; });
        ceiling = std::max(ceiling, max_step(fixed, k_switch_sample, fixed.size()));
    }
    EXPECT_LE(max_step(out, k_switch_sample, out.size()), 2.0f * ceiling);
}

TEST(RingsModelSwitch, PolyphonyChangeDoesNotClick) {
    for (int to = 1; to <= 2; ++to) {
        auto const out = render_switching(k_switch_at + 40, [&](int b) {
            return std::pair{0, b < k_switch_at ? 0 : to};
        });
        EXPECT_LE(switch_step(out), allowed_step(out)) << "polyphony 0 -> " << to;
    }
}

// The fade must end in the new model, not hold the old one.
TEST(RingsModelSwitch, ModelChangesAfterTheFade) {
    constexpr int k_blocks = k_switch_at + 60;
    auto const stayed = render_switching(k_blocks, [](int) { return std::pair{0, 0}; });
    auto const switched =
        render_switching(k_blocks, [](int b) { return std::pair{b < k_switch_at ? 0 : 3, 0}; });
    // Identical up to the switch...
    for (size_t i = 0; i < k_switch_sample; ++i) { ASSERT_EQ(stayed[i], switched[i]) << i; }
    // ...and clearly different 50 ms after it.
    float diff = 0.0f;
    float energy = 0.0f;
    for (size_t i = k_switch_sample + 2400; i < switched.size(); ++i) {
        diff += (switched[i] - stayed[i]) * (switched[i] - stayed[i]);
        energy += stayed[i] * stayed[i];
    }
    EXPECT_GT(diff, 0.1f * energy);
}

TEST_F(RingsResonatorSynthProcessorTest, OddEvenMixAffectsOutput) {
    m_synth.set_float(Parameter::OddEvenMix, 0.0f);
    m_synth.set_float(Parameter::DryWet, 1.0f);

    std::array<float, static_cast<size_t>(k_block_size) * 4> out_zero{};
    for (int b = 0; b < 4; ++b) {
        std::array<float, k_block_size> input = {};
        if (b == 0) { input[0] = 1.0f; }
        float* out_ptr = out_zero.data() + static_cast<size_t>(b) * k_block_size;
        m_synth.process(thl::core::ConstBufferView(input.data(), k_block_size),
                        thl::core::BufferView(out_ptr, k_block_size));
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
        synth2.process(thl::core::ConstBufferView(input.data(), k_block_size),
                       thl::core::BufferView(out_ptr, k_block_size));
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
        m_synth.process(thl::core::ConstBufferView(input.data(), k_block_size),
                        thl::core::BufferView(out_ptr, k_block_size));
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

        synth_a.process(thl::core::ConstBufferView(input.data(), k_block_size),
                        thl::core::BufferView(out_a.data(), k_block_size));
        synth_b.process(thl::core::ConstBufferView(input_copy.data(), k_block_size),
                        thl::core::BufferView(out_b.data(), k_block_size));

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
        m_synth.process(thl::core::ConstBufferView(input.data(), k_block_size),
                        thl::core::BufferView(out1.data(), k_block_size));
        synth2.process(thl::core::ConstBufferView(input2.data(), k_block_size),
                       thl::core::BufferView(out2.data(), k_block_size));

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
        synth.process(thl::core::ConstBufferView(in.data(), kWrapperProcessBlockSize),
                      thl::core::BufferView(out.data(), kWrapperProcessBlockSize));
    }

    for (int b = 0; b < kWrapperNumBlocks; ++b) {
        std::array<float, kWrapperProcessBlockSize> out = {};
        const float* in_ptr = ref_input + static_cast<size_t>(b) * kWrapperProcessBlockSize;

        // Copy input since process() may read from output buffer
        std::array<float, kWrapperProcessBlockSize> in;
        std::copy(in_ptr, in_ptr + kWrapperProcessBlockSize, in.begin());

        synth.process(thl::core::ConstBufferView(in.data(), kWrapperProcessBlockSize),
                      thl::core::BufferView(out.data(), kWrapperProcessBlockSize));

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
