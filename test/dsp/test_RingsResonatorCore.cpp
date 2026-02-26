#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include <tanh/dsp/resonator/rings/Dsp.h>
#include <tanh/dsp/resonator/rings/Part.h>
#include <tanh/dsp/resonator/rings/Patch.h>
#include <tanh/dsp/resonator/rings/PerformanceState.h>
#include <tanh/dsp/resonator/rings/fx/Reverb.h>

#ifdef RINGS_HAS_REFERENCE_FIXTURES
#include <RingsTestFixtures.h>
#endif

namespace rings = thl::dsp::resonator::rings;

namespace {

rings::Patch default_patch() {
    rings::Patch patch {};
    patch.structure = 0.5f;
    patch.brightness = 0.5f;
    patch.damping = 0.3f;
    patch.position = 0.5f;
    return patch;
}

rings::PerformanceState default_state() {
    rings::PerformanceState state {};
    state.strum = false;
    state.internal_exciter = false;
    state.internal_strum = false;
    state.internal_note = false;
    state.tonic = 12.0f;
    state.note = 48.0f;
    state.fm = 0.0f;
    state.chord = 0;
    return state;
}

} // namespace

class RingsResonatorModelTest
    : public ::testing::TestWithParam<rings::ResonatorModel> {};

TEST_P(RingsResonatorModelTest, SilenceInputProducesFiniteOutput) {
    rings::Part part;
    std::memset(&part, 0, sizeof(part));
    std::array<uint16_t, rings::Reverb::kReverbBufferSize> reverb_buffer {};
    part.init(reverb_buffer.data());
    part.set_model(GetParam());

    auto patch = default_patch();
    auto state = default_state();

    std::array<float, rings::kMaxBlockSize> in {};
    std::array<float, rings::kMaxBlockSize> out {};
    std::array<float, rings::kMaxBlockSize> aux {};

    for (int block = 0; block < 8; ++block) {
        std::fill(out.begin(), out.end(), 0.0f);
        std::fill(aux.begin(), aux.end(), 0.0f);
        part.process(state, patch, in.data(), out.data(), aux.data(),
                     rings::kMaxBlockSize);
    }

    for (size_t i = 0; i < rings::kMaxBlockSize; ++i) {
        EXPECT_TRUE(std::isfinite(out[i]));
        EXPECT_TRUE(std::isfinite(aux[i]));
    }
}

TEST_P(RingsResonatorModelTest, ImpulseProducesEnergy) {
    rings::Part part;
    std::memset(&part, 0, sizeof(part));
    std::array<uint16_t, rings::Reverb::kReverbBufferSize> reverb_buffer {};
    part.init(reverb_buffer.data());
    part.set_model(GetParam());

    auto patch = default_patch();
    auto state = default_state();

    std::array<float, rings::kMaxBlockSize> silence {};
    std::array<float, rings::kMaxBlockSize> in {};
    std::array<float, rings::kMaxBlockSize> out {};
    std::array<float, rings::kMaxBlockSize> aux {};

    for (int block = 0; block < 4; ++block) {
        std::fill(out.begin(), out.end(), 0.0f);
        std::fill(aux.begin(), aux.end(), 0.0f);
        part.process(state, patch, silence.data(), out.data(), aux.data(),
                     rings::kMaxBlockSize);
    }

    in.fill(0.0f);
    in[0] = 1.0f;
    part.process(state, patch, in.data(), out.data(), aux.data(),
                 rings::kMaxBlockSize);

    float max_abs = 0.0f;
    float energy = 0.0f;
    for (int block = 0; block < 12; ++block) {
        if (block > 0) {
            std::fill(out.begin(), out.end(), 0.0f);
            std::fill(aux.begin(), aux.end(), 0.0f);
            part.process(state, patch, silence.data(), out.data(), aux.data(),
                         rings::kMaxBlockSize);
        }
        for (size_t i = 0; i < rings::kMaxBlockSize; ++i) {
            max_abs = std::max(max_abs,
                               std::max(std::abs(out[i]), std::abs(aux[i])));
            energy += out[i] * out[i] + aux[i] * aux[i];
            ASSERT_TRUE(std::isfinite(out[i]));
            ASSERT_TRUE(std::isfinite(aux[i]));
        }
    }

    EXPECT_GT(max_abs, 1.0e-4f);
    EXPECT_GT(energy, 1.0e-5f);
}

INSTANTIATE_TEST_SUITE_P(
    AllModels,
    RingsResonatorModelTest,
    ::testing::Values(
        rings::RESONATOR_MODEL_MODAL,
        rings::RESONATOR_MODEL_SYMPATHETIC_STRING,
        rings::RESONATOR_MODEL_STRING,
        rings::RESONATOR_MODEL_FM_VOICE,
        rings::RESONATOR_MODEL_SYMPATHETIC_STRING_QUANTIZED,
        rings::RESONATOR_MODEL_STRING_AND_REVERB),
    [](const ::testing::TestParamInfo<rings::ResonatorModel>& info) {
        switch (info.param) {
            case rings::RESONATOR_MODEL_MODAL: return "Modal";
            case rings::RESONATOR_MODEL_SYMPATHETIC_STRING:
                return "SympatheticString";
            case rings::RESONATOR_MODEL_STRING: return "ModulatedString";
            case rings::RESONATOR_MODEL_FM_VOICE: return "FMVoice";
            case rings::RESONATOR_MODEL_SYMPATHETIC_STRING_QUANTIZED:
                return "SympatheticStringQuantized";
            case rings::RESONATOR_MODEL_STRING_AND_REVERB:
                return "StringAndReverb";
            default: return "Unknown";
        }
    });

#ifdef RINGS_HAS_REFERENCE_FIXTURES

static constexpr int kWarmUpBlocks = 4;
static constexpr int kNumBlocks = 16;
static constexpr size_t kFramesPerBlock = rings::kMaxBlockSize;
static constexpr size_t kTotalFrames = kNumBlocks * kFramesPerBlock;

struct ReferenceModelInfo {
    rings::ResonatorModel model;
    const char* fixture_filename;
};

float reference_tolerance(rings::ResonatorModel model) {
    switch (model) {
        case rings::RESONATOR_MODEL_FM_VOICE:
            // Empirically observed max deviation ~1.4e-2 after LUT replacement.
            return 2e-2f;
        default:
            return 1e-4f;
    }
}

class RingsReferenceOutputTest
    : public ::testing::TestWithParam<ReferenceModelInfo> {};

TEST_P(RingsReferenceOutputTest, MatchesReferenceData) {
    const auto& info = GetParam();
    const float tolerance = reference_tolerance(info.model);

    int size_bytes = 0;
    const char* raw = RingsTestFixtures::getNamedResource(
        info.fixture_filename, size_bytes);
    ASSERT_NE(raw, nullptr)
        << "Missing fixture: " << info.fixture_filename;
    ASSERT_EQ(size_bytes,
              static_cast<int>(kTotalFrames * 2 * sizeof(float)))
        << "Fixture size mismatch for " << info.fixture_filename;

    const float* ref_out = reinterpret_cast<const float*>(raw);
    const float* ref_aux = ref_out + kTotalFrames;

    rings::Part part;
    std::memset(&part, 0, sizeof(part));
    std::array<uint16_t, rings::Reverb::kReverbBufferSize> reverb_buffer {};
    part.init(reverb_buffer.data());
    part.set_model(info.model);

    auto patch = default_patch();
    auto state = default_state();

    // Warm up: run silence to settle uninitialised internal state
    for (int block = 0; block < kWarmUpBlocks; ++block) {
        std::array<float, kFramesPerBlock> in {};
        std::array<float, kFramesPerBlock> out {};
        std::array<float, kFramesPerBlock> aux {};
        part.process(state, patch, in.data(), out.data(), aux.data(),
                     kFramesPerBlock);
    }

    for (int block = 0; block < kNumBlocks; ++block) {
        std::array<float, kFramesPerBlock> in {};
        std::array<float, kFramesPerBlock> out {};
        std::array<float, kFramesPerBlock> aux {};

        if (block == 0) {
            in[0] = 1.0f;
        }

        part.process(state, patch, in.data(), out.data(), aux.data(),
                     kFramesPerBlock);

        for (size_t i = 0; i < kFramesPerBlock; ++i) {
            size_t idx = block * kFramesPerBlock + i;
            EXPECT_NEAR(out[i], ref_out[idx], tolerance)
                << "out mismatch at block " << block << " sample " << i;
            EXPECT_NEAR(aux[i], ref_aux[idx], tolerance)
                << "aux mismatch at block " << block << " sample " << i;
        }
    }
}

INSTANTIATE_TEST_SUITE_P(
    AllModels,
    RingsReferenceOutputTest,
    ::testing::Values(
        ReferenceModelInfo {rings::RESONATOR_MODEL_MODAL, "modal.bin"},
        ReferenceModelInfo {rings::RESONATOR_MODEL_SYMPATHETIC_STRING,
                            "sympathetic_string.bin"},
        ReferenceModelInfo {rings::RESONATOR_MODEL_STRING,
                            "modulated_string.bin"},
        ReferenceModelInfo {rings::RESONATOR_MODEL_FM_VOICE, "fm_voice.bin"},
        ReferenceModelInfo {
            rings::RESONATOR_MODEL_SYMPATHETIC_STRING_QUANTIZED,
            "sympathetic_string_quantized.bin"},
        ReferenceModelInfo {rings::RESONATOR_MODEL_STRING_AND_REVERB,
                            "string_and_reverb.bin"}),
    [](const ::testing::TestParamInfo<ReferenceModelInfo>& info) {
        switch (info.param.model) {
            case rings::RESONATOR_MODEL_MODAL: return "Modal";
            case rings::RESONATOR_MODEL_SYMPATHETIC_STRING:
                return "SympatheticString";
            case rings::RESONATOR_MODEL_STRING: return "ModulatedString";
            case rings::RESONATOR_MODEL_FM_VOICE: return "FMVoice";
            case rings::RESONATOR_MODEL_SYMPATHETIC_STRING_QUANTIZED:
                return "SympatheticStringQuantized";
            case rings::RESONATOR_MODEL_STRING_AND_REVERB:
                return "StringAndReverb";
            default: return "Unknown";
        }
    });

#endif  // RINGS_HAS_REFERENCE_FIXTURES
