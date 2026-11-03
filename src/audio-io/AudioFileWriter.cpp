#include <tanh/audio-io/AudioFileWriter.h>
#include <tanh/core/AbiNamespace.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "MiniaudioFormat.h"
#include "miniaudio.h"

namespace thl::inline THL_ABI_NAMESPACE::audio_io {

namespace {

constexpr size_t k_write_block_frames = 4096;

bool write_interleaved_blocks(ma_encoder& encoder, const core::ConstBufferView& buffer) {
    const size_t num_channels = buffer.get_num_channels();
    const size_t num_samples = buffer.get_num_samples();
    std::vector<float> scratch(k_write_block_frames * num_channels);

    for (size_t block_start = 0; block_start < num_samples; block_start += k_write_block_frames) {
        const size_t block_frames = std::min(k_write_block_frames, num_samples - block_start);
        for (size_t channel = 0; channel < num_channels; ++channel) {
            const float* samples = buffer.get_read_pointer(channel) + block_start;
            for (size_t frame = 0; frame < block_frames; ++frame) {
                scratch[frame * num_channels + channel] = samples[frame];
            }
        }

        ma_uint64 frames_written = 0;
        const ma_result result =
            ma_encoder_write_pcm_frames(&encoder, scratch.data(), block_frames, &frames_written);
        if (result != MA_SUCCESS || frames_written != block_frames) { return false; }
    }
    return true;
}

}  // namespace

bool AudioFileWriter::write_to_file(const std::string& file_path,
                                    core::ConstBufferView buffer,
                                    double sample_rate,
                                    AudioEncodingFormat format) {
    if (buffer.get_num_channels() == 0 || sample_rate <= 0.0) { return false; }

    const ma_encoder_config config =
        ma_encoder_config_init(to_miniaudio_format(format),
                               ma_format_f32,
                               static_cast<ma_uint32>(buffer.get_num_channels()),
                               static_cast<ma_uint32>(sample_rate));

    ma_encoder encoder{};
    if (ma_encoder_init_file(file_path.c_str(), &config, &encoder) != MA_SUCCESS) { return false; }

    const bool written = write_interleaved_blocks(encoder, buffer);
    ma_encoder_uninit(&encoder);
    return written;
}

}  // namespace thl::inline THL_ABI_NAMESPACE::audio_io
