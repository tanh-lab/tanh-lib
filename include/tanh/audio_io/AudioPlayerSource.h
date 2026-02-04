#pragma once
#include "AudioIODeviceCallback.h"
#include "miniaudio.h"
#include <atomic>
#include <functional>
#include <string>

namespace thl {

/**
 * @class AudioPlayerSource
 * @brief Audio callback that plays back audio from a file using async decoding.
 *
 * AudioPlayerSource implements AudioIODeviceCallback to decode audio from a
 * file and output it to the audio device. It uses miniaudio's resource manager
 * for asynchronous streaming and decoding, making the audio thread read from
 * pre-decoded buffers rather than performing blocking file I/O.
 *
 * @section lifecycle File Lifecycle
 *
 * The typical usage pattern is:
 * 1. Construct an AudioPlayerSource
 * 2. Call loadFile() with the audio file path
 * 3. Register with AudioDeviceManager::addCallback()
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
 * manager.addCallback(&player);
 * manager.start();
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
                  ma_uint32 outputChannels,
                  ma_uint32 outputSampleRate);

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
     *
     * @warning NOT real-time safe - seeking may block.
     */
    void stop();

    /**
     * @brief Checks if playback is currently active.
     *
     * @return true if playing, false if paused or stopped.
     *
     * @note Thread-safe - uses atomic operations.
     */
    bool isPlaying() const {
        return m_playing.load(std::memory_order_acquire);
    }

    /**
     * @brief Checks if a file is currently loaded.
     *
     * @return true if a file is loaded and ready for playback, false
     * otherwise.
     */
    bool isLoaded() const { return m_loaded; }

    /**
     * @brief Seeks to a specific frame position.
     *
     * @param frame The target frame position (0 = beginning of file).
     *
     * @warning NOT real-time safe - may trigger buffer refill.
     */
    void seekToFrame(ma_uint64 frame);

    /**
     * @brief Gets the current playback position in frames.
     *
     * @return Current frame position, or 0 if no file is loaded.
     */
    ma_uint64 getCurrentFrame() const;

    /**
     * @brief Gets the total length of the loaded file in frames.
     *
     * @return Total number of frames, or 0 if no file is loaded.
     */
    ma_uint64 getTotalFrames() const;

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
     * @param numChannels Number of output channels.
     *
     * @note This method reads from pre-decoded buffers and is real-time safe.
     */
    void process(float* outputBuffer,
                 const float* inputBuffer,
                 ma_uint32 frameCount,
                 ma_uint32 numChannels) override;

    /**
     * @brief Releases resources by unloading any loaded file.
     *
     * Called by AudioDeviceManager when audio is stopped or when this
     * callback is removed.
     *
     * @warning NOT real-time safe - performs deallocations.
     */
    void releaseResources() override;

private:
    ma_resource_manager m_resourceManager;
    ma_resource_manager_data_source m_dataSource;
    bool m_resourceManagerInitialised = false;
    bool m_loaded = false;
    std::atomic<bool> m_playing{false};
    ma_uint32 m_channels = 0;
    ma_uint32 m_sampleRate = 0;
    FinishedCallback m_finishedCallback;
};

}  // namespace thl
