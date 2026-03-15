#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#include <tanh/dsp/audio/AudioBufferView.h>
#include <tanh/dsp/rings-resonator/RingsDsp.h>
#include <tanh/dsp/rings-resonator/RingsVoiceManager.h>
#include <tanh/dsp/rings-resonator/RingsPatch.h>
#include <tanh/dsp/rings-resonator/RingsPerformanceState.h>
#include <tanh/dsp/rings-resonator/fx/RingsReverb.h>

namespace rings = thl::dsp::synth;

static constexpr int kWarmUpBlocks = 4;
static constexpr int kNumBlocks = 171;
static constexpr size_t kFramesPerBlock = thl::dsp::resonator::kMaxBlockSize;
static constexpr size_t kTotalFrames = kNumBlocks * kFramesPerBlock;

struct ModelInfo {
    rings::ResonatorModel model;
    const char* filename;
};

static constexpr ModelInfo kModels[] = {
    {rings::RESONATOR_MODEL_MODAL, "modal.bin"},
    {rings::RESONATOR_MODEL_SYMPATHETIC_STRING, "sympathetic_string.bin"},
    {rings::RESONATOR_MODEL_STRING, "modulated_string.bin"},
    {rings::RESONATOR_MODEL_FM_VOICE, "fm_voice.bin"},
    {rings::RESONATOR_MODEL_SYMPATHETIC_STRING_QUANTIZED, "sympathetic_string_quantized.bin"},
    {rings::RESONATOR_MODEL_STRING_AND_REVERB, "string_and_reverb.bin"},
};

static thl::dsp::resonator::RingsPatch default_patch() {
    thl::dsp::resonator::RingsPatch patch{};
    patch.structure = 0.5f;
    patch.brightness = 0.5f;
    patch.damping = 0.3f;
    patch.position = 0.5f;
    return patch;
}

static thl::dsp::resonator::RingsPerformanceState default_state() {
    thl::dsp::resonator::RingsPerformanceState state{};
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

int main() {
    namespace fs = std::filesystem;

#ifdef FIXTURE_OUTPUT_DIR
    fs::path output_dir = FIXTURE_OUTPUT_DIR;
#else
    fs::path output_dir = fs::current_path() / "test" / "dsp" / "fixtures";
#endif

    fs::create_directories(output_dir);
    printf("Writing reference data to: %s\n", output_dir.c_str());

    auto patch = default_patch();
    auto state = default_state();

    for (const auto& info : kModels) {
        rings::RingsVoiceManager part;
        std::memset(&part, 0, sizeof(part));
        std::array<uint16_t, thl::dsp::fx::RingsReverb::kReverbBufferSize> reverb_buffer{};
        part.prepare(reverb_buffer.data());
        part.set_model(info.model);

        // Warm up: run silence to settle uninitialised internal state
        for (int block = 0; block < kWarmUpBlocks; ++block) {
            std::array<float, kFramesPerBlock> in{};
            std::array<float, kFramesPerBlock> out{};
            std::array<float, kFramesPerBlock> aux{};
            thl::dsp::audio::ConstAudioBufferView in_view(in.data(), kFramesPerBlock);
            thl::dsp::audio::AudioBufferView out_view(out.data(), kFramesPerBlock);
            thl::dsp::audio::AudioBufferView aux_view(aux.data(), kFramesPerBlock);
            part.process(state, patch, in_view, out_view, aux_view);
        }

        // Build input: impulse at sample 0 of first block
        std::array<float, kTotalFrames> input_data{};
        input_data[0] = 1.0f;

        std::array<float, kTotalFrames> out_data{};
        std::array<float, kTotalFrames> aux_data{};

        for (int block = 0; block < kNumBlocks; ++block) {
            float* in_ptr = input_data.data() + block * kFramesPerBlock;
            float* out_ptr = out_data.data() + block * kFramesPerBlock;
            float* aux_ptr = aux_data.data() + block * kFramesPerBlock;

            thl::dsp::audio::ConstAudioBufferView in_view(in_ptr, kFramesPerBlock);
            thl::dsp::audio::AudioBufferView out_view(out_ptr, kFramesPerBlock);
            thl::dsp::audio::AudioBufferView aux_view(aux_ptr, kFramesPerBlock);
            part.process(state, patch, in_view, out_view, aux_view);
        }

        // Write [input][out][aux] to binary file
        fs::path filepath = output_dir / info.filename;
        FILE* f = fopen(filepath.c_str(), "wb");
        if (!f) {
            fprintf(stderr, "Failed to open %s for writing\n", filepath.c_str());
            return 1;
        }

        fwrite(input_data.data(), sizeof(float), kTotalFrames, f);
        fwrite(out_data.data(), sizeof(float), kTotalFrames, f);
        fwrite(aux_data.data(), sizeof(float), kTotalFrames, f);
        fclose(f);

        printf("  %s (%zu bytes)\n", info.filename, kTotalFrames * 3 * sizeof(float));
    }

    printf("Done.\n");
    return 0;
}
