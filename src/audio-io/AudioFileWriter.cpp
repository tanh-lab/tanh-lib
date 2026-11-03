#include <tanh/audio-io/AudioFileWriter.h>
#include <tanh/core/AbiNamespace.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>
#include <vector>

#include "MiniaudioFormat.h"
#include "miniaudio.h"

namespace thl::inline THL_ABI_NAMESPACE::audio_io {

namespace {

constexpr size_t k_write_block_frames = 4096;
constexpr uint64_t k_bytes_per_sample = sizeof(float);

// RIFF chunk size field = "WAVE" (4) + fmt chunk (8 + 16) + data chunk header (8) + data bytes.
constexpr uint64_t k_riff_overhead_bytes = 36;
constexpr uint64_t k_max_riff_chunk_bytes = std::numeric_limits<uint32_t>::max();

bool is_valid_sample_rate(double sample_rate) {
    return std::isfinite(sample_rate) && sample_rate >= 1.0 &&
           sample_rate <= static_cast<double>(std::numeric_limits<ma_uint32>::max());
}

bool fits_in_riff(size_t num_channels, size_t num_samples) {
    const uint64_t max_data_bytes = k_max_riff_chunk_bytes - k_riff_overhead_bytes;
    const uint64_t frame_bytes = static_cast<uint64_t>(num_channels) * k_bytes_per_sample;
    return static_cast<uint64_t>(num_samples) <= max_data_bytes / frame_bytes;
}

std::ofstream& stream_of(ma_encoder* encoder) {
    return *static_cast<std::ofstream*>(encoder->pUserData);
}

ma_result on_write(ma_encoder* encoder,
                   const void* data,
                   size_t bytes_to_write,
                   size_t* bytes_written) {
    std::ofstream& stream = stream_of(encoder);
    stream.write(static_cast<const char*>(data), static_cast<std::streamsize>(bytes_to_write));
    *bytes_written = stream ? bytes_to_write : 0;
    return stream ? MA_SUCCESS : MA_IO_ERROR;
}

ma_result on_seek(ma_encoder* encoder, ma_int64 offset, ma_seek_origin origin) {
    std::ofstream& stream = stream_of(encoder);
    std::ios_base::seekdir direction = std::ios_base::beg;
    if (origin == ma_seek_origin_current) { direction = std::ios_base::cur; }
    if (origin == ma_seek_origin_end) { direction = std::ios_base::end; }
    stream.seekp(static_cast<std::streamoff>(offset), direction);
    return stream ? MA_SUCCESS : MA_IO_ERROR;
}

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

bool encode_to_stream(std::ofstream& stream,
                      const core::ConstBufferView& buffer,
                      ma_uint32 sample_rate,
                      AudioEncodingFormat format) {
    const ma_encoder_config config =
        ma_encoder_config_init(to_miniaudio_format(format),
                               ma_format_f32,
                               static_cast<ma_uint32>(buffer.get_num_channels()),
                               sample_rate);

    ma_encoder encoder{};
    if (ma_encoder_init(on_write, on_seek, &stream, &config, &encoder) != MA_SUCCESS) {
        return false;
    }

    const bool frames_written = write_interleaved_blocks(encoder, buffer);
    // Finalises the header through on_write/on_seek; any failure leaves the stream failed.
    ma_encoder_uninit(&encoder);
    return frames_written && stream.good();
}

}  // namespace

bool AudioFileWriter::write_to_file(const std::string& file_path,
                                    core::ConstBufferView buffer,
                                    double sample_rate,
                                    AudioEncodingFormat format) {
    const size_t num_channels = buffer.get_num_channels();
    if (format != AudioEncodingFormat::WAV) { return false; }
    if (num_channels == 0 || num_channels > MA_MAX_CHANNELS) { return false; }
    if (!is_valid_sample_rate(sample_rate)) { return false; }
    if (!fits_in_riff(num_channels, buffer.get_num_samples())) { return false; }

    const std::filesystem::path destination(file_path);
    std::filesystem::path partial = destination;
    partial += ".partial";

    std::ofstream stream(partial, std::ios::binary | std::ios::trunc);
    bool written =
        stream.is_open() &&
        encode_to_stream(stream, buffer, static_cast<ma_uint32>(std::llround(sample_rate)), format);
    stream.close();
    written = written && !stream.fail();

    std::error_code error;
    if (written) { std::filesystem::rename(partial, destination, error); }
    if (!written || error) {
        std::filesystem::remove(partial, error);
        return false;
    }
    return true;
}

}  // namespace thl::inline THL_ABI_NAMESPACE::audio_io
