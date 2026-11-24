#pragma once
#include <tanh/audio-io/AudioEncodingFormat.h>
#include <tanh/core/AbiNamespace.h>
#include <tanh/core/Exports.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "AudioIODeviceCallback.h"

namespace thl::inline THL_ABI_NAMESPACE {

/**
 * @class AudioFileSink
 * @brief Audio callback that records input audio to a file.
 *
 * AudioFileSink implements AudioIODeviceCallback to capture audio input and
 * encode it to a file. It supports various audio formats through the backend
 * encoder interface.
 *
 * @par File Lifecycle
 *
 * The typical usage pattern is:
 * 1. Construct an AudioFileSink
 * 2. Call open_file() with the desired path and format
 * 3. Register with AudioDeviceManager::addPlaybackCallback()
 * 4. Call start_recording() to begin capturing audio
 * 5. Call stop_recording() when done
 * 6. Call close_file() or let the destructor handle cleanup
 *
 * @par Real-Time Safety
 *
 * - open_file() and close_file() are NOT real-time safe and should only be
 *   called from the main thread.
 * - process() does no file I/O: it copies the block into a preallocated
 *   buffer, and a writer thread of the sink moves it to the file. A disk that
 *   stalls for longer than the buffer holds (two seconds) costs whole blocks,
 *   counted in get_frames_dropped().
 * - The file is written next to its path under a temporary name and appears
 *   at the path when close_file() has finished it.
 *
 * @par Supported Formats
 *
 * The format parameter in open_file() controls the output file format:
 * - AudioEncodingFormat::WAV (default) - Uncompressed WAV, 32-bit float
 * - AudioEncodingFormat::FLAC - not supported by the bundled encoder:
 *   open_file() fails
 *
 * For several tracks at once, other bit depths, or a recording that starts
 * and stops on a bar line, see audio_io::MultitrackRecorder.
 *
 * @code
 * AudioFileSink recorder;
 * recorder.open_file("recording.wav", 2, 48000);
 * manager.addPlaybackCallback(&recorder);
 * manager.startPlayback();
 * recorder.start_recording();
 * // ... record audio ...
 * recorder.stop_recording();
 * manager.stopPlayback();
 * @endcode
 *
 * @see AudioIODeviceCallback
 * @see AudioPlayerSource for the complementary playback class
 */
class TANH_API AudioFileSink : public AudioIODeviceCallback {
public:
    /**
     * @brief Constructs an AudioFileSink in the closed state.
     */
    AudioFileSink();

    /**
     * @brief Destructs the AudioFileSink, closing any open file.
     *
     * @warning NOT real-time safe - may perform file I/O.
     */
    ~AudioFileSink() override;

    /// @brief Copy constructor (deleted - AudioFileSink is non-copyable)
    AudioFileSink(const AudioFileSink&) = delete;
    /// @brief Copy assignment (deleted - AudioFileSink is non-copyable)
    AudioFileSink& operator=(const AudioFileSink&) = delete;

    /**
     * @brief Opens a file for audio recording.
     *
     * Initialises the encoder and prepares the file for writing. Any
     * previously open file is closed first.
     *
     * @param file_path Path to the output file.
     * @param channels Number of audio channels to record.
     * @param sample_rate Sample rate in Hz.
     * @param format Encoding format (default: WAV).
     *
     * @return true if the file was opened successfully, false otherwise.
     *
     * @warning NOT real-time safe - performs file I/O and allocations.
     */
    bool open_file(const std::string& file_path,
                   uint32_t channels,
                   uint32_t sample_rate,
                   AudioEncodingFormat format = AudioEncodingFormat::WAV);

    /**
     * @brief Closes the currently open file.
     *
     * Stops recording if active, writes what is still buffered and finalises
     * the file. Safe to call even if no file is open.
     *
     * @return true if the file is complete at its path. false if no file was
     *         open, or if a write failed (a full disk): the file is then not
     *         published, and has_failed() is true.
     *
     * @warning NOT real-time safe - performs file I/O.
     */
    bool close_file();

    /**
     * @brief Checks whether the recording could not be written.
     *
     * @return true once a write to the open or last file failed. From then on
     *         process() counts what it is handed as dropped. A file that
     *         reached the 4 GiB WAV limit is not a failure: it ends there and
     *         close_file() publishes it.
     *
     * @note Thread-safe - uses atomic operations.
     */
    bool has_failed() const;

    /**
     * @brief Begins recording audio to the open file.
     *
     * Has no effect if no file is open. Recording state can be toggled
     * without reopening the file.
     *
     * @note Thread-safe - uses atomic operations.
     */
    void start_recording();

    /**
     * @brief Stops recording audio.
     *
     * Audio data received after this call will not be written to the file.
     * The file remains open and recording can be resumed with start_recording().
     *
     * @note Thread-safe - uses atomic operations.
     */
    void stop_recording();

    /**
     * @brief Checks if audio is currently being recorded.
     *
     * @return true if recording is active, false otherwise.
     *
     * @note Thread-safe - uses atomic operations.
     */
    bool is_recording() const { return m_recording.load(std::memory_order_acquire); }

    /**
     * @brief Checks if a file is currently open.
     *
     * @return true if a file is open and ready for recording, false otherwise.
     */
    bool is_open() const { return m_open; }

    /**
     * @brief Gets the total number of frames recorded.
     *
     * @return Number of audio frames accepted since the file was opened; they
     *         are in the file when close_file() returns.
     *
     * @note Thread-safe - uses atomic operations.
     */
    uint64_t get_frames_written() const { return m_frames_written.load(std::memory_order_acquire); }

    /**
     * @brief Gets the number of frames that were left out.
     *
     * @return Frames since the file was opened that found no room in the
     *         sink's buffer because the disk did not keep up, or that came
     *         after the file stopped taking any (has_failed(), or full).
     *
     * @note Thread-safe - uses atomic operations.
     */
    uint64_t get_frames_dropped() const { return m_frames_dropped.load(std::memory_order_acquire); }

    /**
     * @brief Gets the peak amplitude of the most recent audio block.
     *
     * @return Peak level (0.0 to 1.0+) from the last processed block.
     *
     * @note Thread-safe - uses atomic operations.
     */
    float get_peak_level() const { return m_peak_level.load(std::memory_order_acquire); }

    /**
     * @brief Processes audio input and writes to file if recording.
     *
     * If recording is active and a file is open, writes the input buffer
     * to the file. The output buffer is not modified.
     *
     * @param output_buffer Ignored - recording does not produce output.
     * @param input_buffer Audio data to record.
     * @param frame_count Number of frames in the buffer.
     * @param num_input_channels Number of input channels.
     * @param num_output_channels Number of output channels (unused).
     *
     * @note Real-time safe: no file I/O, no allocation, no lock.
     */
    void process(float* output_buffer,
                 const float* input_buffer,
                 uint32_t frame_count,
                 uint32_t num_input_channels,
                 uint32_t num_output_channels) override;

    /**
     * @brief Releases resources by closing any open file.
     *
     * Called by AudioDeviceManager when audio is stopped or when this
     * callback is removed.
     *
     * @warning NOT real-time safe - performs file I/O.
     */
    void release_resources() override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;

    bool m_open = false;
    std::atomic<bool> m_recording{false};
    std::atomic<uint64_t> m_frames_written{0};
    std::atomic<uint64_t> m_frames_dropped{0};
    std::atomic<float> m_peak_level{0.0f};
};

}  // namespace thl::inline THL_ABI_NAMESPACE
