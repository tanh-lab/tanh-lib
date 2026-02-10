#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace thl::audio_io {

/**
 * Thin wrapper around a decoded audio data source.
 *
 * Provides streaming read and seek access to PCM audio data without
 * exposing the underlying audio backend.  The output format is always
 * 32-bit float interleaved.
 *
 * Instances are created via AudioFileLoader::load_data_source_from_file.
 * The class is move-only.
 */
class DataSource {
public:
    ~DataSource();

    DataSource(DataSource&&) noexcept;
    DataSource& operator=(DataSource&&) noexcept;

    DataSource(const DataSource&) = delete;
    DataSource& operator=(const DataSource&) = delete;

    /**
     * Read interleaved PCM frames from the current position.
     *
     * @param output      Destination buffer (must hold at least
     *                    frame_count * channel_count floats).
     * @param frame_count Number of frames to read.
     * @return The number of frames actually read (may be less at EOF).
     */
    uint64_t read_pcm_frames(float* output, uint64_t frame_count);

    /**
     * Seek to an absolute PCM frame position.
     *
     * @param frame_index Target frame index (0-based).
     * @return true on success.
     */
    bool seek(uint64_t frame_index);

    uint32_t get_channel_count() const;
    uint32_t get_sample_rate() const;
    uint64_t get_total_frames() const;
    uint64_t get_cursor() const;

    bool is_valid() const;

private:
    friend class AudioFileLoader;

    struct Impl;
    std::unique_ptr<Impl> m_impl;

    explicit DataSource(std::unique_ptr<Impl> impl);
};

}  // namespace thl::audio_io
