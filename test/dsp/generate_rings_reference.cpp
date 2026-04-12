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

static constexpr int k_warm_up_blocks = 4;
static constexpr int k_num_blocks = 171;
static constexpr size_t k_frames_per_block = thl::dsp::resonator::k_max_block_size;
static constexpr size_t k_total_frames = k_num_blocks * k_frames_per_block;

struct ModelInfo {
    rings::ResonatorModel m_model;
    const char* m_filename;
};

static constexpr std::array k_models = {
    ModelInfo{.m_model = rings::Modal, .m_filename = "modal.bin"},
    ModelInfo{.m_model = rings::SympatheticString, .m_filename = "sympathetic_string.bin"},
    ModelInfo{.m_model = rings::String, .m_filename = "modulated_string.bin"},
    ModelInfo{.m_model = rings::FmVoice, .m_filename = "fm_voice.bin"},
    ModelInfo{.m_model = rings::SympatheticStringQuantized,
              .m_filename = "sympathetic_string_quantized.bin"},
    ModelInfo{.m_model = rings::StringAndReverb, .m_filename = "string_and_reverb.bin"},
};

static thl::dsp::resonator::RingsPatch default_patch() {
    thl::dsp::resonator::RingsPatch patch{};
    patch.m_structure = 0.5f;
    patch.m_brightness = 0.5f;
    patch.m_damping = 0.3f;
    patch.m_position = 0.5f;
    return patch;
}

static thl::dsp::resonator::RingsPerformanceState default_state() {
    thl::dsp::resonator::RingsPerformanceState state{};
    state.m_strum = false;
    state.m_internal_exciter = false;
    state.m_internal_strum = false;
    state.m_internal_note = false;
    state.m_tonic = 12.0f;
    state.m_note = 48.0f;
    state.m_fm = 0.0f;
    state.m_chord = 0;
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
    printf("Writing reference data to: %s\n", output_dir.string().c_str());

    auto patch = default_patch();
    auto state = default_state();

    for (const auto& info : k_models) {
        rings::RingsVoiceManager part;
        std::memset(static_cast<void*>(&part), 0, sizeof(part));
        std::array<uint16_t, thl::dsp::fx::RingsReverb::k_reverb_buffer_size> reverb_buffer{};
        part.prepare(reverb_buffer.data());
        part.set_model(info.m_model);

        // Warm up: run silence to settle uninitialised internal state
        for (int block = 0; block < k_warm_up_blocks; ++block) {
            std::array<float, k_frames_per_block> in{};
            std::array<float, k_frames_per_block> out{};
            std::array<float, k_frames_per_block> aux{};
            thl::dsp::audio::ConstAudioBufferView in_view(in.data(), k_frames_per_block);
            thl::dsp::audio::AudioBufferView out_view(out.data(), k_frames_per_block);
            thl::dsp::audio::AudioBufferView aux_view(aux.data(), k_frames_per_block);
            part.process(state, patch, in_view, out_view, aux_view);
        }

        // Build input: impulse at sample 0 of first block
        std::array<float, k_total_frames> input_data{};
        input_data[0] = 1.0f;

        std::array<float, k_total_frames> out_data{};
        std::array<float, k_total_frames> aux_data{};

        for (int block = 0; block < k_num_blocks; ++block) {
            float* in_ptr = input_data.data() + block * k_frames_per_block;
            float* out_ptr = out_data.data() + block * k_frames_per_block;
            float* aux_ptr = aux_data.data() + block * k_frames_per_block;

            thl::dsp::audio::ConstAudioBufferView in_view(in_ptr, k_frames_per_block);
            thl::dsp::audio::AudioBufferView out_view(out_ptr, k_frames_per_block);
            thl::dsp::audio::AudioBufferView aux_view(aux_ptr, k_frames_per_block);
            part.process(state, patch, in_view, out_view, aux_view);
        }

        // Write [input][out][aux] to binary file
        fs::path filepath = output_dir / info.m_filename;
        FILE* f = fopen(filepath.string().c_str(), "wb");
        if (!f) {
            fprintf(stderr, "Failed to open %s for writing\n", filepath.string().c_str());
            return 1;
        }

        fwrite(input_data.data(), sizeof(float), k_total_frames, f);
        fwrite(out_data.data(), sizeof(float), k_total_frames, f);
        fwrite(aux_data.data(), sizeof(float), k_total_frames, f);
        fclose(f);

        printf("  %s (%zu bytes)\n", info.m_filename, k_total_frames * 3 * sizeof(float));
    }

    printf("Done.\n");
    return 0;
}
