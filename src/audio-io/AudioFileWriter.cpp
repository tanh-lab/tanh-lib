#include <tanh/audio-io/AudioFileWriter.h>
#include <tanh/core/AbiNamespace.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <optional>
#include <random>
#include <string>
#include <system_error>
#include <vector>

#include "MiniaudioFormat.h"
#include "miniaudio.h"

namespace thl::inline THL_ABI_NAMESPACE::audio_io {

namespace {

constexpr size_t k_write_block_frames = 4096;
constexpr uint64_t k_bytes_per_sample = sizeof(float);
constexpr int k_max_staging_attempts = 16;

// Highest rate the bundled WAV decoder accepts, so every written file can be read back.
constexpr double k_max_sample_rate = ma_standard_sample_rate_max;

// RIFF chunk size field = "WAVE" (4) + fmt chunk (8 + 16) + data chunk header (8) + data bytes.
constexpr uint64_t k_riff_overhead_bytes = 36;
constexpr uint64_t k_max_riff_chunk_bytes = std::numeric_limits<uint32_t>::max();

std::optional<ma_uint32> to_supported_sample_rate(double sample_rate) {
    if (!std::isfinite(sample_rate) || sample_rate < 1.0 ||
        sample_rate >= k_max_sample_rate + 0.5) {
        return std::nullopt;
    }
    return static_cast<ma_uint32>(std::llround(sample_rate));
}

bool fits_in_riff(size_t num_channels, size_t num_samples) {
    const uint64_t max_data_bytes = k_max_riff_chunk_bytes - k_riff_overhead_bytes;
    const uint64_t frame_bytes = static_cast<uint64_t>(num_channels) * k_bytes_per_sample;
    return static_cast<uint64_t>(num_samples) <= max_data_bytes / frame_bytes;
}

std::FILE* open_exclusive(const std::filesystem::path& path) {
#ifdef _WIN32
    return _wfopen(path.c_str(), L"wbx");
#else
    return std::fopen(path.c_str(), "wbx");
#endif
}

struct StagingFile {
    std::filesystem::path m_path;
    std::FILE* m_file = nullptr;
};

// Exclusive creation refuses existing paths, including symlinks, so a staging file is never
// shared with another export or redirected elsewhere. The basename is short and independent of
// the destination's, so any destination name that fits the filesystem also leaves room for it.
std::optional<StagingFile> create_staging_file(const std::filesystem::path& destination) {
    std::random_device entropy;
    std::mt19937_64 generator((static_cast<uint64_t>(entropy()) << 32) | entropy());

    for (int attempt = 0; attempt < k_max_staging_attempts; ++attempt) {
        std::array<char, 32> basename{};
        std::snprintf(basename.data(),
                      basename.size(),
                      ".tanh-%016llx.partial",
                      static_cast<unsigned long long>(generator()));
        const std::filesystem::path staging_path = destination.parent_path() / basename.data();

        errno = 0;
        if (std::FILE* file = open_exclusive(staging_path)) {
            return StagingFile{.m_path = staging_path, .m_file = file};
        }
        if (errno != EEXIST) { return std::nullopt; }
    }
    return std::nullopt;
}

// miniaudio ignores the results of the header writes and seeks it makes while finalising, so
// every I/O failure is recorded here and checked before the file is published.
struct EncoderTarget {
    std::FILE* m_file = nullptr;
    bool m_io_failed = false;
};

EncoderTarget& target_of(ma_encoder* encoder) {
    return *static_cast<EncoderTarget*>(encoder->pUserData);
}

ma_result on_write(ma_encoder* encoder,
                   const void* data,
                   size_t bytes_to_write,
                   size_t* bytes_written) {
    EncoderTarget& target = target_of(encoder);
    *bytes_written = std::fwrite(data, 1, bytes_to_write, target.m_file);
    if (*bytes_written == bytes_to_write) { return MA_SUCCESS; }
    target.m_io_failed = true;
    return MA_IO_ERROR;
}

ma_result on_seek(ma_encoder* encoder, ma_int64 offset, ma_seek_origin origin) {
    EncoderTarget& target = target_of(encoder);
    int whence = SEEK_SET;
    if (origin == ma_seek_origin_current) { whence = SEEK_CUR; }
    if (origin == ma_seek_origin_end) { whence = SEEK_END; }
    if (std::fseek(target.m_file, static_cast<long>(offset), whence) == 0) { return MA_SUCCESS; }
    target.m_io_failed = true;
    return MA_IO_ERROR;
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

bool encode_to_file(std::FILE* file,
                    const core::ConstBufferView& buffer,
                    ma_uint32 sample_rate,
                    AudioEncodingFormat format) {
    const ma_encoder_config config =
        ma_encoder_config_init(to_miniaudio_format(format),
                               ma_format_f32,
                               static_cast<ma_uint32>(buffer.get_num_channels()),
                               sample_rate);

    EncoderTarget target{.m_file = file};
    ma_encoder encoder{};
    if (ma_encoder_init(on_write, on_seek, &target, &config, &encoder) != MA_SUCCESS) {
        return false;
    }

    const bool frames_written = write_interleaved_blocks(encoder, buffer);
    ma_encoder_uninit(&encoder);
    return frames_written && !target.m_io_failed && std::ferror(file) == 0;
}

}  // namespace

bool AudioFileWriter::write_to_file(const std::string& file_path,
                                    core::ConstBufferView buffer,
                                    double sample_rate,
                                    AudioEncodingFormat format) {
    const size_t num_channels = buffer.get_num_channels();
    if (format != AudioEncodingFormat::WAV) { return false; }
    if (num_channels == 0 || num_channels > MA_MAX_CHANNELS) { return false; }
    if (!fits_in_riff(num_channels, buffer.get_num_samples())) { return false; }

    const std::optional<ma_uint32> supported_rate = to_supported_sample_rate(sample_rate);
    if (!supported_rate) { return false; }

    const std::filesystem::path destination(file_path);
    std::optional<StagingFile> staging = create_staging_file(destination);
    if (!staging) { return false; }

    bool written = encode_to_file(staging->m_file, buffer, *supported_rate, format);
    written = std::fclose(staging->m_file) == 0 && written;

    std::error_code error;
    if (written) { std::filesystem::rename(staging->m_path, destination, error); }
    if (!written || error) {
        std::filesystem::remove(staging->m_path, error);
        return false;
    }
    return true;
}

}  // namespace thl::inline THL_ABI_NAMESPACE::audio_io
