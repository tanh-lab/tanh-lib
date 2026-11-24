#include <tanh/audio-io/WavStreamWriter.h>
#include <tanh/core/AbiNamespace.h>
#include <tanh/core/BufferView.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "miniaudio.h"

namespace thl::inline THL_ABI_NAMESPACE::audio_io {

namespace {

constexpr size_t k_write_block_frames = 4096;
constexpr int k_max_staging_attempts = 16;

// Highest rate the bundled WAV decoder accepts, so every written file can be read back.
constexpr double k_max_sample_rate = ma_standard_sample_rate_max;

// RIFF chunk size field = "WAVE" (4) + fmt chunk (8 + 16) + data chunk header (8) + data bytes
// + the pad byte that follows data of odd length.
constexpr uint64_t k_riff_overhead_bytes = 36 + 1;
constexpr uint64_t k_max_riff_chunk_bytes = std::numeric_limits<uint32_t>::max();

std::optional<ma_uint32> to_supported_sample_rate(double sample_rate) {
    if (!std::isfinite(sample_rate) || sample_rate < 1.0 ||
        sample_rate >= k_max_sample_rate + 0.5) {
        return std::nullopt;
    }
    return static_cast<ma_uint32>(std::llround(sample_rate));
}

ma_format to_miniaudio_sample_format(SampleFormat format) {
    switch (format) {
        case SampleFormat::Pcm16: return ma_format_s16;
        case SampleFormat::Pcm24: return ma_format_s24;
        case SampleFormat::Float32: return ma_format_f32;
    }
    return ma_format_f32;
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

// Not-a-number has no place in a file: it becomes silence.
float finite_or_zero(float sample) {
    return std::isfinite(sample) ? sample : 0.0f;
}

}  // namespace

struct WavStreamWriter::Impl {
    WavStreamConfig m_config;
    std::filesystem::path m_destination;
    std::filesystem::path m_staging_path;
    EncoderTarget m_target;
    ma_encoder m_encoder{};
    bool m_open = false;
    bool m_failed = false;
    uint64_t m_frames_written = 0;
    uint64_t m_max_frames = 0;
    uint32_t m_dither_state = 0x9E3779B9U;
    std::vector<float> m_interleave;  // planar blocks and silence, k_write_block_frames frames
    std::vector<uint8_t> m_encoded;   // integer formats, k_write_block_frames frames

    // Uniform in [0, 1): xorshift32, 24 bits of it.
    float next_uniform() {
        m_dither_state ^= m_dither_state << 13;
        m_dither_state ^= m_dither_state >> 17;
        m_dither_state ^= m_dither_state << 5;
        return static_cast<float>(m_dither_state >> 8) * (1.0f / 16777216.0f);
    }

    void encode_pcm16(const float* samples, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            float value = finite_or_zero(samples[i]) * 32768.0f;
            if (m_config.m_dither) { value += next_uniform() - next_uniform(); }
            const float rounded = std::clamp(std::floor(value + 0.5f), -32768.0f, 32767.0f);
            const auto bits = static_cast<uint16_t>(static_cast<int16_t>(rounded));
            m_encoded[i * 2] = static_cast<uint8_t>(bits & 0xFFU);
            m_encoded[(i * 2) + 1] = static_cast<uint8_t>(bits >> 8);
        }
    }

    void encode_pcm24(const float* samples, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            const double value = static_cast<double>(finite_or_zero(samples[i])) * 8388608.0;
            const double rounded = std::clamp(std::floor(value + 0.5), -8388608.0, 8388607.0);
            const auto bits = static_cast<uint32_t>(static_cast<int32_t>(rounded));
            m_encoded[i * 3] = static_cast<uint8_t>(bits & 0xFFU);
            m_encoded[(i * 3) + 1] = static_cast<uint8_t>((bits >> 8) & 0xFFU);
            m_encoded[(i * 3) + 2] = static_cast<uint8_t>((bits >> 16) & 0xFFU);
        }
    }

    // One block of at most k_write_block_frames interleaved frames.
    bool write_block(const float* interleaved, size_t num_frames) {
        const void* data = interleaved;
        const size_t num_values = num_frames * m_config.m_channels;
        if (m_config.m_format == SampleFormat::Pcm16) {
            encode_pcm16(interleaved, num_values);
            data = m_encoded.data();
        } else if (m_config.m_format == SampleFormat::Pcm24) {
            encode_pcm24(interleaved, num_values);
            data = m_encoded.data();
        }

        ma_uint64 frames_written = 0;
        const ma_result result =
            ma_encoder_write_pcm_frames(&m_encoder, data, num_frames, &frames_written);
        if (result != MA_SUCCESS || frames_written != num_frames || m_target.m_io_failed) {
            m_failed = true;
            return false;
        }
        m_frames_written += num_frames;
        return true;
    }

    [[nodiscard]] bool accepts(uint64_t num_frames) const {
        return m_open && !m_failed && num_frames <= m_max_frames - m_frames_written;
    }

    void discard() {
        if (!m_open) { return; }
        ma_encoder_uninit(&m_encoder);
        std::fclose(m_target.m_file);
        std::error_code error;
        std::filesystem::remove(m_staging_path, error);
        m_target = {};
        m_open = false;
    }
};

WavStreamWriter::WavStreamWriter() : m_impl(std::make_unique<Impl>()) {}

WavStreamWriter::~WavStreamWriter() {
    if (m_impl) { m_impl->discard(); }
}

WavStreamWriter::WavStreamWriter(WavStreamWriter&&) noexcept = default;

WavStreamWriter& WavStreamWriter::operator=(WavStreamWriter&& other) noexcept {
    if (this != &other) {
        if (m_impl) { m_impl->discard(); }
        m_impl = std::move(other.m_impl);
    }
    return *this;
}

uint64_t WavStreamWriter::max_frames(uint32_t channels, SampleFormat format) noexcept {
    if (channels == 0) { return 0; }
    const uint64_t max_data_bytes = k_max_riff_chunk_bytes - k_riff_overhead_bytes;
    return max_data_bytes / (static_cast<uint64_t>(channels) * bytes_per_sample(format));
}

bool WavStreamWriter::open(const std::string& file_path, const WavStreamConfig& config) {
    if (!m_impl) { m_impl = std::make_unique<Impl>(); }
    m_impl->discard();

    if (config.m_channels == 0 || config.m_channels > MA_MAX_CHANNELS) { return false; }
    const std::optional<ma_uint32> sample_rate = to_supported_sample_rate(config.m_sample_rate);
    if (!sample_rate) { return false; }

    const std::filesystem::path destination(file_path);
    std::optional<StagingFile> staging = create_staging_file(destination);
    if (!staging) { return false; }

    const size_t block_values = k_write_block_frames * config.m_channels;
    m_impl->m_config = config;
    m_impl->m_destination = destination;
    m_impl->m_staging_path = staging->m_path;
    m_impl->m_target = EncoderTarget{.m_file = staging->m_file};
    m_impl->m_failed = false;
    m_impl->m_frames_written = 0;
    m_impl->m_max_frames = max_frames(config.m_channels, config.m_format);
    m_impl->m_dither_state = 0x9E3779B9U;
    m_impl->m_interleave.assign(block_values, 0.0f);
    m_impl->m_encoded.assign(config.m_format == SampleFormat::Float32
                                 ? 0
                                 : block_values * bytes_per_sample(config.m_format),
                             0);

    const ma_encoder_config encoder_config =
        ma_encoder_config_init(ma_encoding_format_wav,
                               to_miniaudio_sample_format(config.m_format),
                               config.m_channels,
                               *sample_rate);
    if (ma_encoder_init(on_write,
                        on_seek,
                        &m_impl->m_target,
                        &encoder_config,
                        &m_impl->m_encoder) != MA_SUCCESS) {
        std::fclose(staging->m_file);
        std::error_code error;
        std::filesystem::remove(staging->m_path, error);
        m_impl->m_target = {};
        return false;
    }
    m_impl->m_open = true;
    return true;
}

bool WavStreamWriter::write(const float* interleaved, uint64_t num_frames) {
    if (!m_impl || !m_impl->accepts(num_frames)) { return false; }
    if (num_frames == 0) { return true; }
    if (interleaved == nullptr) { return false; }

    const size_t channels = m_impl->m_config.m_channels;
    uint64_t done = 0;
    while (done < num_frames) {
        const auto frames =
            static_cast<size_t>(std::min<uint64_t>(k_write_block_frames, num_frames - done));
        if (!m_impl->write_block(interleaved + (done * channels), frames)) { return false; }
        done += frames;
    }
    return true;
}

bool WavStreamWriter::write(const core::ConstBufferView& block) {
    if (!m_impl || !m_impl->m_open) { return false; }
    const size_t channels = m_impl->m_config.m_channels;
    const size_t num_frames = block.get_num_samples();
    if (block.get_num_channels() != channels || !m_impl->accepts(num_frames)) { return false; }

    float* scratch = m_impl->m_interleave.data();
    for (size_t start = 0; start < num_frames; start += k_write_block_frames) {
        const size_t frames = std::min(k_write_block_frames, num_frames - start);
        for (size_t channel = 0; channel < channels; ++channel) {
            const float* samples = block.get_read_pointer(channel) + start;
            for (size_t frame = 0; frame < frames; ++frame) {
                scratch[(frame * channels) + channel] = samples[frame];
            }
        }
        if (!m_impl->write_block(scratch, frames)) { return false; }
    }
    return true;
}

bool WavStreamWriter::write_silence(uint64_t num_frames) {
    if (!m_impl || !m_impl->accepts(num_frames)) { return false; }
    std::ranges::fill(m_impl->m_interleave, 0.0f);
    uint64_t done = 0;
    while (done < num_frames) {
        const auto frames =
            static_cast<size_t>(std::min<uint64_t>(k_write_block_frames, num_frames - done));
        if (!m_impl->write_block(m_impl->m_interleave.data(), frames)) { return false; }
        done += frames;
    }
    return true;
}

bool WavStreamWriter::close() {
    if (!m_impl || !m_impl->m_open) { return false; }

    ma_encoder_uninit(&m_impl->m_encoder);
    bool written = !m_impl->m_failed && !m_impl->m_target.m_io_failed &&
                   std::ferror(m_impl->m_target.m_file) == 0;
    written = std::fclose(m_impl->m_target.m_file) == 0 && written;
    m_impl->m_target = {};
    m_impl->m_open = false;

    std::error_code error;
    if (written) { std::filesystem::rename(m_impl->m_staging_path, m_impl->m_destination, error); }
    if (!written || error) {
        std::filesystem::remove(m_impl->m_staging_path, error);
        return false;
    }
    return true;
}

void WavStreamWriter::abort() {
    if (m_impl) { m_impl->discard(); }
}

bool WavStreamWriter::is_open() const noexcept {
    return m_impl && m_impl->m_open;
}

uint64_t WavStreamWriter::frames_written() const noexcept {
    return m_impl ? m_impl->m_frames_written : 0;
}

uint64_t WavStreamWriter::frames_left() const noexcept {
    if (!m_impl || !m_impl->m_open) { return 0; }
    return m_impl->m_max_frames - m_impl->m_frames_written;
}

const WavStreamConfig& WavStreamWriter::config() const noexcept {
    static const WavStreamConfig k_default;
    return m_impl ? m_impl->m_config : k_default;
}

}  // namespace thl::inline THL_ABI_NAMESPACE::audio_io
