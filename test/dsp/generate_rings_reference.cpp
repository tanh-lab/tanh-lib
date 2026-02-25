#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#include <tanh/dsp/resonator/rings/Dsp.h>
#include <tanh/dsp/resonator/rings/Part.h>
#include <tanh/dsp/resonator/rings/Patch.h>
#include <tanh/dsp/resonator/rings/PerformanceState.h>

namespace rings = thl::dsp::resonator::rings;

static constexpr int kWarmUpBlocks = 4;
static constexpr int kNumBlocks = 16;
static constexpr size_t kFramesPerBlock = rings::kMaxBlockSize;
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
    {rings::RESONATOR_MODEL_SYMPATHETIC_STRING_QUANTIZED,
     "sympathetic_string_quantized.bin"},
    {rings::RESONATOR_MODEL_STRING_AND_REVERB, "string_and_reverb.bin"},
};

static rings::Patch default_patch() {
    rings::Patch patch {};
    patch.structure = 0.5f;
    patch.brightness = 0.5f;
    patch.damping = 0.3f;
    patch.position = 0.5f;
    return patch;
}

static rings::PerformanceState default_state() {
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
        rings::Part part;
        std::memset(&part, 0, sizeof(part));
        std::array<uint16_t, 32768> reverb_buffer {};
        part.Init(reverb_buffer.data());
        part.set_model(info.model);

        // Warm up: run silence to settle uninitialised internal state
        for (int block = 0; block < kWarmUpBlocks; ++block) {
            std::array<float, kFramesPerBlock> in {};
            std::array<float, kFramesPerBlock> out {};
            std::array<float, kFramesPerBlock> aux {};
            part.Process(state, patch, in.data(), out.data(), aux.data(),
                         kFramesPerBlock);
        }

        std::array<float, kTotalFrames> out_data {};
        std::array<float, kTotalFrames> aux_data {};

        for (int block = 0; block < kNumBlocks; ++block) {
            std::array<float, kFramesPerBlock> in {};
            if (block == 0) {
                in[0] = 1.0f;
            }

            float* out_ptr = out_data.data() + block * kFramesPerBlock;
            float* aux_ptr = aux_data.data() + block * kFramesPerBlock;

            part.Process(state, patch, in.data(), out_ptr, aux_ptr,
                         kFramesPerBlock);
        }

        fs::path filepath = output_dir / info.filename;
        FILE* f = fopen(filepath.c_str(), "wb");
        if (!f) {
            fprintf(stderr, "Failed to open %s for writing\n",
                    filepath.c_str());
            return 1;
        }

        fwrite(out_data.data(), sizeof(float), kTotalFrames, f);
        fwrite(aux_data.data(), sizeof(float), kTotalFrames, f);
        fclose(f);

        printf("  %s (%zu bytes)\n", info.filename,
               kTotalFrames * 2 * sizeof(float));
    }

    printf("Done.\n");
    return 0;
}
