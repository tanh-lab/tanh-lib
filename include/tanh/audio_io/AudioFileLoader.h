#pragma once

#include <tanh/audio_io/DataSource.h>
#include <tanh/dsp/audio/AudioBuffer.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace thl::audio_io {

/**
 * Loads and decodes audio files using miniaudio.
 *
 * Supports WAV, MP3, and FLAC from both file paths and in-memory buffers.
 * Automatically resamples to a target sample rate and converts to a target
 * channel count when requested.  Output is a planar AudioBuffer.
 *
 * This class has no mutable state, so multiple instances may be used
 * concurrently on different threads.
 */
class AudioFileLoader {
public:
    AudioFileLoader() = default;
    ~AudioFileLoader() = default;

    /**
     * Decode an audio file from a file path.
     *
     * @param file_path          Path to the audio file.
     * @param target_sample_rate Desired output sample rate (0 = keep native).
     * @param target_channels    Desired output channel count (0 = keep native).
     * @return Planar AudioBuffer, or an empty buffer on failure.
     */
    dsp::audio::AudioBuffer load_from_file(const std::string& file_path,
                                           double target_sample_rate = 0.0,
                                           uint32_t target_channels = 0);

    /**
     * Decode audio from an in-memory buffer (e.g. embedded binary data).
     *
     * @param data               Pointer to the binary audio data.
     * @param size               Size of the data in bytes.
     * @param target_sample_rate Desired output sample rate (0 = keep native).
     * @param target_channels    Desired output channel count (0 = keep native).
     * @return Planar AudioBuffer, or an empty buffer on failure.
     */
    dsp::audio::AudioBuffer load_from_memory(const void* data,
                                             size_t size,
                                             double target_sample_rate = 0.0,
                                             uint32_t target_channels = 0);

    /**
     * Open an audio file and return a streaming DataSource.
     *
     * Unlike load_from_file(), this does not decode the entire file into
     * memory.  The caller reads PCM data incrementally via the returned
     * DataSource.
     *
     * @param file_path          Path to the audio file.
     * @param target_sample_rate Desired output sample rate (0 = keep native).
     * @param target_channels    Desired output channel count (0 = keep native).
     * @return A DataSource wrapping the file, or an invalid DataSource on
     *         failure (check DataSource::is_valid()).
     */
    DataSource load_data_source_from_file(const std::string& file_path,
                                          double target_sample_rate = 0.0,
                                          uint32_t target_channels = 0);

    /**
     * Open an in-memory audio buffer and return a streaming DataSource.
     *
     * Unlike load_from_memory(), this does not decode the entire buffer
     * upfront.  The caller reads PCM data incrementally via the returned
     * DataSource.
     *
     * @note The caller must keep the memory pointed to by @p data alive
     *       for the lifetime of the returned DataSource.
     *
     * @param data               Pointer to the binary audio data.
     * @param size               Size of the data in bytes.
     * @param target_sample_rate Desired output sample rate (0 = keep native).
     * @param target_channels    Desired output channel count (0 = keep native).
     * @return A DataSource wrapping the memory, or an invalid DataSource on
     *         failure (check DataSource::is_valid()).
     */
    DataSource load_data_source_from_memory(const void* data,
                                            size_t size,
                                            double target_sample_rate = 0.0,
                                            uint32_t target_channels = 0);

private:
    static bool init_resource_manager_data_source(DataSource::Impl& impl,
                                                  const std::string& file_path,
                                                  double target_sample_rate,
                                                  uint32_t target_channels,
                                                  uint32_t flags);

    static dsp::audio::AudioBuffer read_all_frames(DataSource::Impl& impl);
};

}  // namespace thl::audio_io
