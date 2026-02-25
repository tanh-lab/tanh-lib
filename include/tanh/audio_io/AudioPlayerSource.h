#pragma once
#include "AudioIODeviceCallback.h"
#include <tanh/audio_io/AudioFileLoader.h>
#include <tanh/audio_io/DataSource.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <memory>
#include <string>

namespace thl {

/**
 * @class AudioPlayerSource
 * @brief Audio callback that plays back audio from a file using async decoding.
 *
 * AudioPlayerSource implements AudioIODeviceCallback to decode audio from a
 * file and output it to the audio device. It uses the backend's resource
 * manager for asynchronous streaming and decoding, making the audio thread read
 * from pre-decoded buffers rather than performing blocking file I/O.
 *
 * @section lifecycle File Lifecycle
 *
 * The typical usage pattern is:
 * 1. Construct an AudioPlayerSource
 * 2. Call loadFile() with the audio file path
 * 3. Register with AudioDeviceManager::addPlaybackCallback()
 * 4. Call play() to begin playback
 * 5. Call pause() or stop() to control playback
 * 6. Call unloadFile() or let the destructor handle cleanup
 *
 * @section playback Playback Control
 *
 * - play() - Starts or resumes playback from the current position
 * - pause() - Pauses playback, maintaining the current position
 * - stop() - Stops playback and resets to the beginning
 * - seekToFrame() - Seeks to a specific frame position
 *
 * @section rt_safety Real-Time Safety
 *
 * - loadFile(), unloadFile(), and seekToFrame() are NOT real-time safe.
 * - play(), pause(), and stop() use atomic operations and are safe to call
 *   from any thread, though stop() also performs a seek operation.
 * - process() reads from pre-decoded buffers managed by the resource manager's
 *   background thread, making it suitable for real-time audio processing.
 *
 * @section finished_callback Finished Callback
 *
 * Use setFinishedCallback() to be notified when playback reaches the end of
 * the file. The callback is invoked from the audio thread, so it must be
 * real-time safe (no allocations, locks, or blocking).
 *
 * @code
 * AudioPlayerSource player;
 * player.loadFile("audio.wav", 2, 48000);
 * player.setFinishedCallback([]() {
 *     // Handle end of file (must be RT-safe!)
 * });
 * manager.addPlaybackCallback(&player);
 * manager.startPlayback();
 * player.play();
 * @endcode
 *
 * @see AudioIODeviceCallback
 * @see AudioFileSink for the complementary recording class
 */
class AudioPlayerSource : public AudioIODeviceCallback {
public:
    /**
     * @brief Callback type invoked when playback reaches the end of the file.
     *
     * @warning The callback is invoked from the audio thread and MUST be
     *          real-time safe.
     */
    using FinishedCallback = std::function<void()>;

    /**
     * @brief Constructs an AudioPlayerSource with no file loaded.
     *
     * Initialises the internal resource manager for async decoding.
     */
    AudioPlayerSource();

    /**
     * @brief Destructs the AudioPlayerSource, unloading any file.
     *
     * @warning NOT real-time safe - waits for background threads to complete.
     */
    ~AudioPlayerSource() override;

    /// @brief Copy constructor (deleted - AudioPlayerSource is non-copyable)
    AudioPlayerSource(const AudioPlayerSource&) = delete;
    /// @brief Copy assignment (deleted - AudioPlayerSource is non-copyable)
    AudioPlayerSource& operator=(const AudioPlayerSource&) = delete;

    /**
     * @brief Loads an audio file for playback with async streaming.
     *
     * Initialises the data source with streaming enabled. The resource manager
     * will decode audio on a background thread, allowing the audio thread to
     * read from pre-decoded buffers.
     *
     * @param filePath Path to the audio file to load.
     * @param outputChannels Number of output channels (audio will be
     *                       converted if needed).
     * @param outputSampleRate Output sample rate in Hz (audio will be
     *                         resampled if needed).
     *
     * @return true if the file was loaded successfully, false otherwise.
     *
     * @warning NOT real-time safe - performs file I/O and allocations.
     */
    bool loadFile(const std::string& filePath,
                  uint32_t outputChannels,
                  uint32_t outputSampleRate);

    /**
     * @brief Loads audio from an in-memory buffer for playback.
     *
     * Initialises a streaming data source from the provided memory buffer.
     * The caller must keep the memory alive for the lifetime of this player
     * (or until unloadFile() / loadFile() / loadFromMemory() is called).
     *
     * @param data            Pointer to the binary audio data.
     * @param size            Size of the data in bytes.
     * @param outputChannels  Number of output channels (audio will be
     *                        converted if needed).
     * @param outputSampleRate Output sample rate in Hz (audio will be
     *                         resampled if needed).
     *
     * @return true if the data was loaded successfully, false otherwise.
     *
     * @warning NOT real-time safe - performs allocations.
     */
    bool loadFromMemory(const void* data,
                        size_t size,
                        uint32_t outputChannels,
                        uint32_t outputSampleRate);

    /**
     * @brief Unloads the currently loaded file.
     *
     * Stops playback and releases decoder resources. Safe to call even if
     * no file is loaded.
     *
     * @warning NOT real-time safe - waits for background decoding to complete.
     */
    void unloadFile();

    /**
     * @brief Starts or resumes playback.
     *
     * Has no effect if no file is loaded. Playback begins from the current
     * position.
     *
     * @note Thread-safe - uses atomic operations.
     */
    void play();

    /**
     * @brief Pauses playback.
     *
     * Playback can be resumed from the current position with play().
     *
     * @note Thread-safe - uses atomic operations.
     */
    void pause();

    /**
     * @brief Stops playback and resets to the beginning.
     *
     * Equivalent to pause() followed by seekToFrame(0).
     * This is a hard stop — audio output stops immediately.
     * Use requestStop() for a click-free fade-out.
     *
     * @warning NOT real-time safe - seeking may block.
     */
    void stop();

    /**
     * @brief Requests a click-free stop with a short fade-out (~1 ms).
     *
     * Signals the audio thread to ramp the output to zero over kFadeSamples
     * frames. When the fade completes, playback is stopped and the finished
     * callback is invoked. Safe to call from any thread.
     *
     * @note Thread-safe — uses atomic operations. The actual stop happens
     *       asynchronously on the audio thread.
     */
    void requestStop();

    /**
     * @brief Checks if playback is currently active.
     *
     * @return true if playing, false if paused or stopped.
     *
     * @note Thread-safe - uses atomic operations.
     */
    bool isPlaying() const { return m_playing.load(std::memory_order_acquire); }

    /**
     * @brief Checks if a file is currently loaded.
     *
     * @return true if a file is loaded and ready for playback, false
     * otherwise.
     */
    bool isLoaded() const { return m_loaded.load(std::memory_order_acquire); }

    /**
     * @brief Seeks to a specific frame position.
     *
     * @param frame The target frame position (0 = beginning of file).
     *
     * @warning NOT real-time safe - may trigger buffer refill.
     */
    void seekToFrame(uint64_t frame);

    /**
     * @brief Gets the current playback position in frames.
     *
     * @return Current frame position, or 0 if no file is loaded.
     */
    uint64_t getCurrentFrame() const;

    /**
     * @brief Gets the total length of the loaded file in frames.
     *
     * @return Total number of frames, or 0 if no file is loaded.
     */
    uint64_t getTotalFrames() const;

    /**
     * @brief Sets a callback to be invoked when playback finishes.
     *
     * The callback is invoked from the audio thread when playback reaches
     * the end of the file. To receive another notification, playback must
     * be restarted (e.g., with stop() followed by play()).
     *
     * @param callback The callback function, or nullptr to clear.
     *
     * @warning The callback runs on the audio thread and MUST be real-time
     *          safe. Do not perform allocations, locks, or blocking
     * operations.
     */
    void setFinishedCallback(FinishedCallback callback);

    void prepareToPlay(uint32_t sampleRate, uint32_t bufferSize) override;

    /**
     * @brief Processes audio by reading from pre-decoded buffers.
     *
     * If playing and a file is loaded, reads audio frames from the resource
     * manager's pre-decoded buffer and writes them to the output buffer.
     * When the end of file is reached, remaining samples are zeroed and the
     * finished callback is invoked.
     *
     * @param outputBuffer Buffer to fill with decoded audio.
     * @param inputBuffer Ignored - playback does not use input.
     * @param frameCount Number of frames to read.
     * @param numInputChannels Number of input channels (unused).
     * @param numOutputChannels Number of output channels.
     *
     * @note This method reads from pre-decoded buffers and is real-time safe.
     */
    void process(float* outputBuffer,
                 const float* inputBuffer,
                 uint32_t frameCount,
                 uint32_t numInputChannels,
                 uint32_t numOutputChannels) override;

    /**
     * @brief Releases resources by unloading any loaded file.
     *
     * Called by AudioDeviceManager when audio is stopped or when this
     * callback is removed.
     *
     * @warning NOT real-time safe - performs deallocations.
     */
    void releaseResources() override;

    /// Number of frames used for micro fade-in/out (~1.3 ms at 48 kHz).
    static constexpr uint32_t kFadeSamples = 64;

    void setFadeEnabled(bool enabled) { m_fadeEnabled = enabled; }
    bool isFadeEnabled() const { return m_fadeEnabled; }

private:
    bool rebuildDataSource(uint32_t decodedChannels,
                           uint32_t decodedSampleRate,
                           uint64_t initialFrame);

    audio_io::AudioFileLoader m_loader;
    std::shared_ptr<audio_io::DataSource> m_dataSource;

    std::atomic<bool> m_loaded{false};
    std::atomic<bool> m_playing{false};
    std::mutex m_stateMutex;

    std::string m_filePath;
    const void* m_memoryData = nullptr;
    size_t m_memorySize = 0;
    uint32_t m_channels = 0;
    uint32_t m_sampleRate = 0;

    std::shared_ptr<FinishedCallback> m_finishedCallback;

    // Fade state — written by control thread, consumed by audio thread
    std::atomic<uint32_t> m_fadeInRemaining{0};
    std::atomic<bool> m_stopRequested{false};
    uint32_t m_fadeOutCounter{0};  // audio-thread only
    bool m_fadeEnabled{true};
};

}  // namespace thl
