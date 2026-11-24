#pragma once

#include <tanh/core/AbiNamespace.h>
#include <tanh/core/BufferView.h>
#include <tanh/core/Exports.h>

#include <cstdint>
#include <memory>
#include <string>

namespace thl::inline THL_ABI_NAMESPACE::audio_io {

/// How a WAV file stores its samples.
enum class SampleFormat : uint8_t {
    Pcm16,   ///< 16-bit integer.
    Pcm24,   ///< 24-bit integer.
    Float32  ///< 32-bit float: what the engine computes, no clipping at full scale.
};

/// Bytes one sample of @p format takes in the file.
[[nodiscard]] constexpr uint32_t bytes_per_sample(SampleFormat format) noexcept {
    switch (format) {
        case SampleFormat::Pcm16: return 2;
        case SampleFormat::Pcm24: return 3;
        case SampleFormat::Float32: return 4;
    }
    return 4;
}

/// The file a WavStreamWriter writes.
struct WavStreamConfig {
    uint32_t m_channels = 2;
    double m_sample_rate = 48000.0;
    SampleFormat m_format = SampleFormat::Float32;
    /// Pcm16 only: triangular dither of one step before rounding, so quiet passages do not
    /// turn into distortion. The dither is reproducible: the same input gives the same file.
    bool m_dither = true;
};

/**
 * Writes a RIFF WAV file block by block, for audio whose length is not known when the file
 * is opened (a recording) or that should not be held in memory at once (an export).
 *
 * The audio goes to a uniquely named, exclusively created staging file next to the
 * destination and is renamed over it by close() only when every write, seek and the close
 * itself have succeeded. A destination that already exists is replaced only then; an abort
 * or a failure leaves it as it was.
 *
 * Input is 32-bit float; the integer formats clip at full scale.
 *
 * A file holds at most 4 GiB (the RIFF limit): frames_left() says how many frames still
 * fit, and a write of more than that fails without writing anything.
 *
 * Not real-time safe: every call may block on the file system. One thread at a time.
 */
class TANH_API WavStreamWriter {
public:
    WavStreamWriter();
    /// abort()s a file that is still open.
    ~WavStreamWriter();

    WavStreamWriter(WavStreamWriter&&) noexcept;
    WavStreamWriter& operator=(WavStreamWriter&&) noexcept;
    WavStreamWriter(const WavStreamWriter&) = delete;
    WavStreamWriter& operator=(const WavStreamWriter&) = delete;

    /// The frames a file of @p channels channels in @p format can hold; 0 for 0 channels.
    [[nodiscard]] static uint64_t max_frames(uint32_t channels, SampleFormat format) noexcept;

    /**
     * Starts a file that close() will publish at @p file_path. A file that is still open is
     * abort()ed first.
     *
     * Returns false without touching the file system when the channel count is 0 or above
     * what the encoder supports, or the sample rate does not round to [1, 384000] Hz; false
     * as well when the staging file cannot be created (the directory must exist).
     */
    bool open(const std::string& file_path, const WavStreamConfig& config);

    /// Appends @p num_frames interleaved frames. False when no file is open, an earlier
    /// write failed, the frames do not fit (frames_left()) or the file system refuses.
    bool write(const float* interleaved, uint64_t num_frames);

    /// Appends a planar block. Its channel count must be the file's.
    bool write(const core::ConstBufferView& block);

    /// Appends @p num_frames frames of silence.
    bool write_silence(uint64_t num_frames);

    /// Finishes the header and publishes the file at its path. On false the staging file is
    /// removed and the destination is as it was before open().
    bool close();

    /// Drops the file: the staging file is removed, the destination stays as it was.
    void abort();

    /// True between a successful open() and close() or abort().
    [[nodiscard]] bool is_open() const noexcept;
    /// Frames accepted since open().
    [[nodiscard]] uint64_t frames_written() const noexcept;
    /// Frames that still fit before the 4 GiB limit; 0 when no file is open.
    [[nodiscard]] uint64_t frames_left() const noexcept;
    /// The configuration of the open file (the last one after close()).
    [[nodiscard]] const WavStreamConfig& config() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace thl::inline THL_ABI_NAMESPACE::audio_io
