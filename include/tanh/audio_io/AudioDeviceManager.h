#pragma once
#include "AudioDeviceInfo.h"
#include "AudioIODeviceCallback.h"
#include <tanh/core/threading/RCU.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace thl {

/**
 * @enum DeviceNotificationType
 * @brief Types of device notification events.
 */
enum class DeviceNotificationType {
    Started,
    Stopped,
    Rerouted,
    InterruptionBegan,
    InterruptionEnded,
    Unlocked
};

/**
 * @class AudioDeviceManager
 * @brief Manages audio device initialisation, lifecycle, and callback dispatch.
 *
 * AudioDeviceManager provides a high-level interface for audio I/O using
 * a cross-platform audio backend. It handles device enumeration,
 * initialisation, and dispatches audio data to registered callbacks.
 *
 * @section lifecycle Device Lifecycle
 *
 * The typical usage pattern is:
 * 1. Construct an AudioDeviceManager (initialises the audio context)
 * 2. Enumerate available devices using enumerateInputDevices() /
 * enumerateOutputDevices()
 * 3. Call initialise() with the desired device configuration
 * 4. Register callbacks using addPlaybackCallback(), addCaptureCallback(),
 *    and/or addDuplexCallback()
 * 5. Call startPlayback() (and optionally startCapture()/startDuplex())
 *    to begin audio processing
 * 6. Call stopPlayback()/stopCapture()/stopDuplex() when done
 * 7. Call shutdown() to release the device (or let destructor handle it)
 *
 * @section threading Threading Model
 *
 * - Device enumeration, initialisation, start/stop, and callback registration
 *   are performed on the calling thread (typically the main thread).
 * - Audio processing (AudioIODeviceCallback::process()) occurs on a dedicated
 *   audio thread created by the audio backend.
 * - Callback registration is protected by a mutex and is safe to call from
 *   any thread, though adding/removing callbacks during audio processing
 *   may cause brief audio glitches.
 *
 * @section rt_safety Real-Time Safety
 *
 * Only the internal processCallbacks() method runs on the audio thread.
 * All public methods are called from the main thread and are NOT real-time
 * safe. Registered callbacks must implement process() in a real-time safe
 * manner.
 *
 * @section ios_rerouting iOS Audio Rerouting
 *
 * On iOS, audio routing is managed by AVAudioSession. When the route changes
 * (e.g., headphones connected/disconnected, Bluetooth device paired), the
 * sample rate, buffer size, or channel count may change. Use
 * setDeviceNotificationCallback() to receive DeviceNotificationType::Rerouted
 * notifications and query the new configuration via getSampleRate(),
 * getBufferSize(), etc. Note that process() may receive varying frameCount
 * values after a route change.
 *
 * @code
 * AudioDeviceManager manager;
 *
 * auto outputs = manager.enumerateOutputDevices();
 * if (!outputs.empty()) {
 *     manager.initialise(nullptr, &outputs[0], 48000, 256, 0, 2);
 *     manager.addPlaybackCallback(&myProcessor);
 *     manager.startPlayback();
 *     // ... audio is now running ...
 *     manager.stopPlayback();
 * }
 * @endcode
 *
 * @see AudioIODeviceCallback
 * @see AudioDeviceInfo
 */
class AudioDeviceManager {
public:
    /**
     * @brief Callback type for device notification events.
     *
     * This callback is invoked when device events occur, such as the device
     * being started, stopped, or disconnected.
     *
     * @see setDeviceNotificationCallback()
     */
    using DeviceNotificationCallback =
        std::function<void(DeviceNotificationType)>;

    /**
     * @brief Callback type for log messages from the audio backend.
     *
     * @param level Log level (0=debug, 1=info, 2=warning, 3=error)
     * @param message The log message
     */
    using LogCallback =
        std::function<void(uint32_t level, const char* message)>;

    /**
     * @brief Constructs an AudioDeviceManager and initialises the audio
     * context.
     *
     * The audio context is initialised automatically, enabling device
     * enumeration. Check isContextInitialised() to verify successful
     * initialisation.
     *
     * @warning NOT real-time safe - performs system calls and allocations.
     */
    AudioDeviceManager();

    /**
     * @brief Destructs the AudioDeviceManager, stopping and releasing all
     * resources.
     *
     * Automatically calls shutdown() if a device is initialised.
     *
     * @warning NOT real-time safe - performs system calls and deallocations.
     */
    ~AudioDeviceManager();

    /// @brief Copy constructor (deleted - AudioDeviceManager is non-copyable)
    AudioDeviceManager(const AudioDeviceManager&) = delete;
    /// @brief Copy assignment (deleted - AudioDeviceManager is non-copyable)
    AudioDeviceManager& operator=(const AudioDeviceManager&) = delete;

    /**
     * @brief Checks if the audio context was successfully initialised.
     *
     * @return true if the context is ready for device operations, false
     * otherwise.
     *
     * @note If this returns false, device enumeration and initialisation will
     * fail.
     */
    bool isContextInitialised() const;

    /**
     * @brief Enumerates all available audio input (capture) devices.
     *
     * Queries the system for available input devices and returns their
     * information including supported sample rates.
     *
     * @return Vector of AudioDeviceInfo structures for each available input
     * device. Returns an empty vector if no devices are available or context is
     * not initialised.
     *
     * @warning NOT real-time safe - performs system queries.
     */
    std::vector<AudioDeviceInfo> enumerateInputDevices() const;

    /**
     * @brief Enumerates all available audio output (playback) devices.
     *
     * Queries the system for available output devices and returns their
     * information including supported sample rates.
     *
     * @return Vector of AudioDeviceInfo structures for each available output
     * device. Returns an empty vector if no devices are available or context is
     * not initialised.
     *
     * @warning NOT real-time safe - performs system queries.
     */
    std::vector<AudioDeviceInfo> enumerateOutputDevices() const;

    /**
     * @brief Initialises the audio device with the specified configuration.
     *
     * Sets up separate playback, capture, and (when both devices are provided)
     * duplex audio devices depending on which device pointers are provided.
     *
     * @param inputDevice Pointer to the input device info, or nullptr for
     * output-only.
     * @param outputDevice Pointer to the output device info, or nullptr for
     * input-only.
     * @param sampleRate Desired sample rate in Hz (default: 44100).
     * @param bufferSizeInFrames Desired buffer size in frames (default: 512).
     *                           Lower values reduce latency but increase CPU
     * load.
     * @param numInputChannels Number of input audio channels (default: 1).
     * @param numOutputChannels Number of output audio channels (default: 1).
     *
     * @return true if initialisation succeeded, false otherwise.
     *
     * @note At least one of inputDevice or outputDevice must be non-null.
     * @note The actual buffer size may differ from the requested size depending
     *       on the audio driver.
     *
     * @warning NOT real-time safe - performs system calls and allocations.
     * @warning Must call shutdown() before re-initialising with different
     * settings.
     */
    bool initialise(const AudioDeviceInfo* inputDevice,
                    const AudioDeviceInfo* outputDevice,
                    uint32_t sampleRate = 44100,
                    uint32_t bufferSizeInFrames = 512,
                    uint32_t numInputChannels = 1,
                    uint32_t numOutputChannels = 1);

    /**
     * @brief Shuts down the audio device and releases associated resources.
     *
     * Stops audio processing if running and releases the audio device.
     * Safe to call multiple times or on an uninitialised manager.
     *
     * @warning NOT real-time safe - performs system calls and deallocations.
     */
    void shutdown();

    /**
     * @brief Starts playback audio processing.
     *
     * Begins audio output on the playback device. Playback callbacks will
     * receive process() calls on the audio thread.
     *
     * @return true if the device was started successfully, false otherwise.
     *
     * @pre initialise() must have been called successfully.
     *
     * @warning NOT real-time safe - may block while waiting for audio thread to
     * start.
     */
    bool startPlayback();

    /**
     * @brief Stops playback audio processing.
     *
     * Halts audio output on the playback device. Playback callbacks will
     * receive releaseResources() after audio processing stops.
     *
     * @warning NOT real-time safe - may block while waiting for audio thread to
     * stop.
     */
    void stopPlayback();

    /**
     * @brief Starts capture audio processing.
     *
     * Begins audio input on the capture device. Capture callbacks will
     * receive process() calls on the audio thread.
     *
     * @return true if the device was started successfully, false otherwise.
     *
     * @pre initialise() must have been called successfully.
     *
     * @warning NOT real-time safe - may block while waiting for audio thread to
     * start.
     */
    bool startCapture();

    /**
     * @brief Stops capture audio processing.
     *
     * Halts audio input on the capture device. Capture callbacks will
     * receive releaseResources() after audio processing stops.
     *
     * @warning NOT real-time safe - may block while waiting for audio thread to
     * stop.
     */
    void stopCapture();

    /**
     * @brief Starts duplex audio processing.
     *
     * Begins audio input and output on the duplex device. Duplex callbacks
     * will receive process() calls on the audio thread.
     *
     * @return true if the device was started successfully, false otherwise.
     *
     * @pre initialise() must have been called successfully.
     *
     * @warning NOT real-time safe - may block while waiting for audio thread to
     * start.
     */
    bool startDuplex();

    /**
     * @brief Stops duplex audio processing.
     *
     * Halts audio input and output on the duplex device. Duplex callbacks will
     * receive releaseResources() after audio processing stops.
     *
     * @warning NOT real-time safe - may block while waiting for audio thread to
     * stop.
     */
    void stopDuplex();

    /**
     * @brief Checks if playback processing is currently running.
     *
     * @return true if playback is actively processing audio, false otherwise.
     */
    bool isPlaybackRunning() const { return m_playbackRunning; }

    /**
     * @brief Checks if capture processing is currently running.
     *
     * @return true if capture is actively processing audio, false otherwise.
     */
    bool isCaptureRunning() const { return m_captureRunning; }

    /**
     * @brief Checks if duplex processing is currently running.
     *
     * @return true if duplex is actively processing audio, false otherwise.
     */
    bool isDuplexRunning() const { return m_duplexRunning; }

    /**
     * @brief Registers a playback audio callback to receive audio data.
     *
     * The callback's process() method will be called on the playback audio
     * thread for each buffer of audio data. Multiple callbacks can be
     * registered and will be called in registration order.
     *
     * @param callback Pointer to the callback to register. Must remain valid
     *                 until removed or the manager is destroyed.
     *
     * @note If the playback device is already initialised, prepareToPlay()
     *       will be called on the callback before it starts receiving
     *       process() calls.
     * @note Thread-safe - uses RCU for lock-free audio thread access.
     *
     * @warning NOT real-time safe - performs allocation.
     */
    void addPlaybackCallback(AudioIODeviceCallback* callback);

    /**
     * @brief Registers a capture audio callback to receive audio data.
     *
     * The callback's process() method will be called on the capture audio
     * thread for each buffer of audio data.
     *
     * @param callback Pointer to the callback to register. Must remain valid
     *                 until removed or the manager is destroyed.
     *
     * @note If the capture device is already initialised, prepareToPlay()
     *       will be called on the callback before it starts receiving
     *       process() calls.
     * @note Thread-safe - uses RCU for lock-free audio thread access.
     *
     * @warning NOT real-time safe - performs allocation.
     */
    void addCaptureCallback(AudioIODeviceCallback* callback);

    /**
     * @brief Registers a duplex audio callback to receive audio data.
     *
     * The callback's process() method will be called on the duplex audio
     * thread for each buffer of audio data.
     *
     * @param callback Pointer to the callback to register. Must remain valid
     *                 until removed or the manager is destroyed.
     *
     * @note If the duplex device is already initialised, prepareToPlay()
     *       will be called on the callback before it starts receiving
     *       process() calls.
     * @note Thread-safe - uses RCU for lock-free audio thread access.
     *
     * @warning NOT real-time safe - performs allocation.
     */
    void addDuplexCallback(AudioIODeviceCallback* callback);

    /**
     * @brief Unregisters a playback audio callback.
     *
     * Removes the callback from the list of registered callbacks. The
     * callback's releaseResources() method will be called if playback is
     * currently running.
     *
     * @param callback Pointer to the callback to remove.
     *
     * @note Safe to call with a callback that is not registered (no-op).
     * @note Thread-safe - uses RCU for lock-free audio thread access.
     *
     * @warning NOT real-time safe - performs allocation.
     */
    void removePlaybackCallback(AudioIODeviceCallback* callback);

    /**
     * @brief Unregisters a capture audio callback.
     *
     * Removes the callback from the list of registered callbacks. The
     * callback's releaseResources() method will be called if capture is
     * currently running.
     *
     * @param callback Pointer to the callback to remove.
     *
     * @note Safe to call with a callback that is not registered (no-op).
     * @note Thread-safe - uses RCU for lock-free audio thread access.
     *
     * @warning NOT real-time safe - performs allocation.
     */
    void removeCaptureCallback(AudioIODeviceCallback* callback);

    /**
     * @brief Unregisters a duplex audio callback.
     *
     * Removes the callback from the list of registered callbacks. The
     * callback's releaseResources() method will be called if duplex is
     * currently running.
     *
     * @param callback Pointer to the callback to remove.
     *
     * @note Safe to call with a callback that is not registered (no-op).
     * @note Thread-safe - uses RCU for lock-free audio thread access.
     *
     * @warning NOT real-time safe - performs allocation.
     */
    void removeDuplexCallback(AudioIODeviceCallback* callback);

    /**
     * @brief Sets a callback for device notification events.
     *
     * The notification callback is invoked when device events occur, such as:
     * - DeviceNotificationType::Started - Device started
     * - DeviceNotificationType::Stopped - Device stopped
     * - DeviceNotificationType::Rerouted - Device rerouted
     * - DeviceNotificationType::InterruptionBegan/InterruptionEnded
     *
     * @param callback The callback function to invoke on notifications,
     *                 or nullptr to disable notifications.
     *
     * @warning NOT real-time safe - modifies internal state.
     */
    void setDeviceNotificationCallback(DeviceNotificationCallback callback);

    /**
     * @brief Sets a callback for log messages from the audio backend.
     *
     * This is post-init only. Logs emitted during context initialization
     * will not be captured.
     *
     * @param callback Log callback function, or nullptr to clear.
     */
    void setLogCallback(LogCallback callback);

    /**
     * @brief Gets the current sample rate.
     *
     * @return The actual device sample rate in Hz if initialised, otherwise
     *         the default (44100).
     */
    uint32_t getSampleRate() const;

    /**
     * @brief Gets the current buffer size.
     *
     * @return The requested buffer size in frames, or the default (512) if not
     *         initialised.
     *
     * @note This is the requested buffer size; the actual size may differ
     *       depending on the audio driver.
     */
    uint32_t getBufferSize() const;

    /**
     * @brief Gets the current number of input (capture) channels.
     *
     * @return The actual capture channel count if initialised, otherwise the
     *         requested input channel count.
     */
    uint32_t getNumInputChannels() const;

    /**
     * @brief Gets the current number of output (playback) channels.
     *
     * @return The actual playback channel count if initialised, otherwise the
     *         requested output channel count.
     */
    uint32_t getNumOutputChannels() const;

private:
    enum class DeviceRole { Playback, Capture, Duplex };
    struct Impl;
    struct DeviceUserData;

    std::unique_ptr<Impl> m_impl;

    std::vector<AudioDeviceInfo> enumerateDevices(DeviceType type) const;
    void populateSampleRates(std::vector<AudioDeviceInfo>& devices) const;

    bool tryInitialiseDevice(DeviceRole role,
                             const AudioDeviceInfo* inputDevice,
                             const AudioDeviceInfo* outputDevice,
                             uint32_t sampleRate,
                             uint32_t bufferSizeInFrames,
                             uint32_t numInputChannels,
                             uint32_t numOutputChannels);

    void processCallbacks(DeviceRole role,
                          void* device,
                          float* output,
                          const float* input,
                          uint32_t frameCount);

    static void dataCallback(void* pDevice,
                             void* pOutput,
                             const void* pInput,
                             uint32_t frameCount);

    static void notificationCallback(const void* pNotification);

    static void staticLogCallback(void* pUserData,
                                  uint32_t level,
                                  const char* pMessage);

    bool m_playbackRunning = false;
    bool m_captureRunning = false;
    bool m_duplexRunning = false;

    uint32_t m_sampleRate = 44100;
    uint32_t m_bufferSize = 512;
    uint32_t m_numInputChannels = 1;
    uint32_t m_numOutputChannels = 1;

    RCU<std::vector<AudioIODeviceCallback*>> m_playbackCallbacks;
    RCU<std::vector<AudioIODeviceCallback*>> m_captureCallbacks;
    RCU<std::vector<AudioIODeviceCallback*>> m_duplexCallbacks;

    mutable std::atomic<bool> m_playbackAudioThreadRegistered{false};
    mutable std::atomic<bool> m_captureAudioThreadRegistered{false};
    mutable std::atomic<bool> m_duplexAudioThreadRegistered{false};

    std::shared_ptr<DeviceNotificationCallback> m_notificationCallback;
};

}  // namespace thl
