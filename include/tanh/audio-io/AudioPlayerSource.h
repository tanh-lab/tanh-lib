#pragma once
#include "AudioIODeviceCallback.h"
#include <tanh/audio-io/AudioFileLoader.h>
#include <tanh/audio-io/DataSource.h>
#include <tanh/core/AtomicSharedPtr.h>
#include <tanh/core/Exports.h>
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
 * 2. Call load_file() with the audio file path
 * 3. Register with AudioDeviceManager::addPlaybackCallback()
 * 4. Call play() to begin playback
 * 5. Call pause() or stop() to control playback
 * 6. Call unload_file() or let the destructor handle cleanup
 *
 * @section playback Playback Control
 *
 * - play() - Starts or resumes playback from the current position
 * - pause() - Pauses playback, maintaining the current position
 * - stop() - Stops playback and resets to the beginning
 * - seek_to_frame() - Seeks to a specific frame position
 *
 * @section rt_safety Real-Time Safety
 *
 * - load_file(), unload_file(), and seek_to_frame() are NOT real-time safe.
 * - play(), pause(), and stop() use atomic operations and are safe to call
 *   from any thread, though stop() also performs a seek operation.
 * - process() reads from pre-decoded buffers managed by the resource manager's
 *   background thread, making it suitable for real-time audio processing.
 *
 * @section finished_callback Finished Callback
 *
 * Use set_finished_callback() to be notified when playback reaches the end of
 * the file. The callback is invoked from the audio thread, so it must be
 * real-time safe (no allocations, locks, or blocking).
 *
 * @code
 * AudioPlayerSource player;
 * player.load_file("audio.wav", 2, 48000);
 * player.set_finished_callback([]() {
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
class TANH_API AudioPlayerSource : public AudioIODeviceCallback {
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
    bool load_file(const std::string& file_path,
                   uint32_t output_channels,
                   uint32_t output_sample_rate);

    /**
     * @brief Loads audio from an in-memory buffer for playback.
     *
     * Initialises a streaming data source from the provided memory buffer.
     * The caller must keep the memory alive for the lifetime of this player
     * (or until unload_file() / load_file() / load_from_memory() is called).
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
    bool load_from_memory(const void* data,
                          size_t size,
                          uint32_t output_channels,
                          uint32_t output_sample_rate);

    /**
     * @brief Unloads the currently loaded file.
     *
     * Stops playback and releases decoder resources. Safe to call even if
     * no file is loaded.
     *
     * @warning NOT real-time safe - waits for background decoding to complete.
     */
    void unload_file();

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
     * Equivalent to pause() followed by seek_to_frame(0).
     * This is a hard stop — audio output stops immediately.
     * Use request_stop() for a click-free fade-out.
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
    void request_stop();

    /**
     * @brief Checks if playback is currently active.
     *
     * @return true if playing, false if paused or stopped.
     *
     * @note Thread-safe - uses atomic operations.
     */
    bool is_playing() const { return m_playing.load(std::memory_order_acquire); }

    /**
     * @brief Checks if a file is currently loaded.
     *
     * @return true if a file is loaded and ready for playback, false
     * otherwise.
     */
    bool is_loaded() const { return m_loaded.load(std::memory_order_acquire); }

    /**
     * @brief Seeks to a specific frame position.
     *
     * @param frame The target frame position (0 = beginning of file).
     *
     * @warning NOT real-time safe - may trigger buffer refill.
     */
    void seek_to_frame(uint64_t frame);

    /**
     * @brief Gets the current playback position in frames.
     *
     * @return Current frame position, or 0 if no file is loaded.
     */
    uint64_t get_current_frame() const;

    /**
     * @brief Gets the total length of the loaded file in frames.
     *
     * @return Total number of frames, or 0 if no file is loaded.
     */
    uint64_t get_total_frames() const;

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
    void set_finished_callback(FinishedCallback callback);

    void prepare_to_play(uint32_t sample_rate, uint32_t buffer_size) override;

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
    void process(float* output_buffer,
                 const float* input_buffer,
                 uint32_t frame_count,
                 uint32_t num_input_channels,
                 uint32_t num_output_channels) override;

    /**
     * @brief Releases resources by unloading any loaded file.
     *
     * Called by AudioDeviceManager when audio is stopped or when this
     * callback is removed.
     *
     * @warning NOT real-time safe - performs deallocations.
     */
    void release_resources() override;

    /// Number of frames used for micro fade-in/out (~1.3 ms at 48 kHz).
    static constexpr uint32_t k_fade_samples = 64;

    void set_fade_enabled(bool enabled) { m_fade_enabled = enabled; }
    bool is_fade_enabled() const { return m_fade_enabled; }

private:
    bool rebuild_data_source(uint32_t decoded_channels,
                             uint32_t decoded_sample_rate,
                             uint64_t initial_frame);

    audio_io::AudioFileLoader m_loader;
    AtomicSharedPtr<audio_io::DataSource> m_data_source;

    std::atomic<bool> m_loaded{false};
    std::atomic<bool> m_playing{false};
    std::mutex m_state_mutex;

    std::string m_file_path;
    const void* m_memory_data = nullptr;
    size_t m_memory_size = 0;
    uint32_t m_channels = 0;
    uint32_t m_sample_rate = 0;

    AtomicSharedPtr<FinishedCallback> m_finished_callback;

    // Fade state — written by control thread, consumed by audio thread
    std::atomic<uint32_t> m_fade_in_remaining{0};
    std::atomic<bool> m_stop_requested{false};
    uint32_t m_fade_out_counter{0};  // audio-thread only
    bool m_fade_enabled{true};
};

}  // namespace thl
