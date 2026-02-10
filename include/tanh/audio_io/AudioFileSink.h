#pragma once
#include "AudioIODeviceCallback.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace thl {

/**
 * @enum AudioEncodingFormat
 * @brief Supported audio file encoding formats.
 */
enum class AudioEncodingFormat { WAV, FLAC };

/**
 * @class AudioFileSink
 * @brief Audio callback that records input audio to a file.
 *
 * AudioFileSink implements AudioIODeviceCallback to capture audio input and
 * encode it to a file. It supports various audio formats through the backend
 * encoder interface.
 *
 * @section lifecycle File Lifecycle
 *
 * The typical usage pattern is:
 * 1. Construct an AudioFileSink
 * 2. Call openFile() with the desired path and format
 * 3. Register with AudioDeviceManager::addPlaybackCallback()
 * 4. Call startRecording() to begin capturing audio
 * 5. Call stopRecording() when done
 * 6. Call closeFile() or let the destructor handle cleanup
 *
 * @section rt_safety Real-Time Safety
 *
 * - openFile(), closeFile(), startRecording(), and stopRecording() are NOT
 *   real-time safe and should only be called from the main thread.
 * - process() writes to the file which involves I/O. While the write itself
 *   may block, this is typically acceptable for recording scenarios. For
 *   strict real-time requirements, consider buffering to a lock-free queue.
 *
 * @section formats Supported Formats
 *
 * The format parameter in openFile() controls the output file format:
 * - AudioEncodingFormat::WAV (default) - Uncompressed WAV
 * - AudioEncodingFormat::FLAC - FLAC lossless compression
 *
 * @code
 * AudioFileSink recorder;
 * recorder.openFile("recording.wav", 2, 48000);
 * manager.addPlaybackCallback(&recorder);
 * manager.startPlayback();
 * recorder.startRecording();
 * // ... record audio ...
 * recorder.stopRecording();
 * manager.stopPlayback();
 * @endcode
 *
 * @see AudioIODeviceCallback
 * @see AudioPlayerSource for the complementary playback class
 */
class AudioFileSink : public AudioIODeviceCallback {
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
     * @param filePath Path to the output file.
     * @param channels Number of audio channels to record.
     * @param sampleRate Sample rate in Hz.
     * @param format Encoding format (default: WAV).
     *
     * @return true if the file was opened successfully, false otherwise.
     *
     * @warning NOT real-time safe - performs file I/O and allocations.
     */
    bool openFile(const std::string& filePath,
                  uint32_t channels,
                  uint32_t sampleRate,
                  AudioEncodingFormat format = AudioEncodingFormat::WAV);

    /**
     * @brief Closes the currently open file.
     *
     * Stops recording if active and finalises the file. Safe to call
     * even if no file is open.
     *
     * @warning NOT real-time safe - performs file I/O.
     */
    void closeFile();

    /**
     * @brief Begins recording audio to the open file.
     *
     * Has no effect if no file is open. Recording state can be toggled
     * without reopening the file.
     *
     * @note Thread-safe - uses atomic operations.
     */
    void startRecording();

    /**
     * @brief Stops recording audio.
     *
     * Audio data received after this call will not be written to the file.
     * The file remains open and recording can be resumed with startRecording().
     *
     * @note Thread-safe - uses atomic operations.
     */
    void stopRecording();

    /**
     * @brief Checks if audio is currently being recorded.
     *
     * @return true if recording is active, false otherwise.
     *
     * @note Thread-safe - uses atomic operations.
     */
    bool isRecording() const {
        return m_recording.load(std::memory_order_acquire);
    }

    /**
     * @brief Checks if a file is currently open.
     *
     * @return true if a file is open and ready for recording, false otherwise.
     */
    bool isOpen() const { return m_open; }

    /**
     * @brief Gets the total number of frames written to the file.
     *
     * @return Number of audio frames written since the file was opened.
     *
     * @note Thread-safe - uses atomic operations.
     */
    uint64_t getFramesWritten() const {
        return m_framesWritten.load(std::memory_order_acquire);
    }

    /**
     * @brief Processes audio input and writes to file if recording.
     *
     * If recording is active and a file is open, writes the input buffer
     * to the file. The output buffer is not modified.
     *
     * @param outputBuffer Ignored - recording does not produce output.
     * @param inputBuffer Audio data to record.
     * @param frameCount Number of frames in the buffer.
     * @param numChannels Number of channels (unused - uses configured value).
     *
     * @note This method performs file I/O and may block.
     */
    void process(float* outputBuffer,
                 const float* inputBuffer,
                 uint32_t frameCount,
                 uint32_t numInputChannels,
                 uint32_t numOutputChannels) override;

    /**
     * @brief Releases resources by closing any open file.
     *
     * Called by AudioDeviceManager when audio is stopped or when this
     * callback is removed.
     *
     * @warning NOT real-time safe - performs file I/O.
     */
    void releaseResources() override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;

    bool m_open = false;
    std::atomic<bool> m_recording{false};
    std::atomic<uint64_t> m_framesWritten{0};
};

}  // namespace thl
