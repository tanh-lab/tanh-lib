#pragma once
#include "miniaudio.h"
#include "AudioDeviceInfo.h"
#include "AudioIODeviceCallback.h"
#include <tanh/core/threading/RCU.h>
#include <atomic>
#include <memory>
#include <vector>
#include <functional>

namespace thl {

/**
 * @class AudioDeviceManager
 * @brief Manages audio device initialisation, lifecycle, and callback dispatch.
 *
 * AudioDeviceManager provides a high-level interface for audio I/O using
 * miniaudio as the backend. It handles device enumeration, initialisation, and
 * dispatches audio data to registered callbacks.
 *
 * @section lifecycle Device Lifecycle
 *
 * The typical usage pattern is:
 * 1. Construct an AudioDeviceManager (initialises the audio context)
 * 2. Enumerate available devices using enumerateInputDevices() /
 * enumerateOutputDevices()
 * 3. Call initialise() with the desired device configuration
 * 4. Register callbacks using addCallback()
 * 5. Call start() to begin audio processing
 * 6. Call stop() when done
 * 7. Call shutdown() to release the device (or let destructor handle it)
 *
 * @section threading Threading Model
 *
 * - Device enumeration, initialisation, start/stop, and callback registration
 *   are performed on the calling thread (typically the main thread).
 * - Audio processing (AudioIODeviceCallback::process()) occurs on a dedicated
 *   audio thread created by miniaudio.
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
 * @code
 * AudioDeviceManager manager;
 *
 * auto outputs = manager.enumerateOutputDevices();
 * if (!outputs.empty()) {
 *     manager.initialise(nullptr, &outputs[0], 48000, 256, 0, 2);
 *     manager.addCallback(&myProcessor);
 *     manager.start();
 *     // ... audio is now running ...
 *     manager.stop();
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
     * @see ma_device_notification_type for possible notification types
     */
    using DeviceNotificationCallback =
        std::function<void(ma_device_notification_type)>;

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
    bool isContextInitialised() const { return m_contextInitialised; }

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
     * Sets up either a playback-only, capture-only, or full-duplex audio device
     * depending on which device pointers are provided.
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
                    ma_uint32 sampleRate = 44100,
                    ma_uint32 bufferSizeInFrames = 512,
                    ma_uint32 numInputChannels = 1,
                    ma_uint32 numOutputChannels = 1);

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
     * @brief Starts audio processing.
     *
     * Begins audio I/O on the initialised device. All registered callbacks
     * will receive prepareToPlay() before audio processing begins, and
     * process() calls will start on the audio thread.
     *
     * @return true if the device was started successfully, false otherwise.
     *
     * @pre initialise() must have been called successfully.
     *
     * @warning NOT real-time safe - may block while waiting for audio thread to
     * start.
     */
    bool start();

    /**
     * @brief Stops audio processing.
     *
     * Halts audio I/O on the device. All registered callbacks will receive
     * releaseResources() after audio processing stops. The device remains
     * initialised and can be restarted with start().
     *
     * @warning NOT real-time safe - may block while waiting for audio thread to
     * stop.
     */
    void stop();

    /**
     * @brief Checks if audio processing is currently running.
     *
     * @return true if the device is actively processing audio, false otherwise.
     */
    bool isRunning() const { return m_running; }

    /**
     * @brief Registers an audio callback to receive audio data.
     *
     * The callback's process() method will be called on the audio thread
     * for each buffer of audio data. Multiple callbacks can be registered
     * and will be called in registration order.
     *
     * @param callback Pointer to the callback to register. Must remain valid
     *                 until removed or the manager is destroyed.
     *
     * @note If audio is already running, prepareToPlay() will be called on
     *       the callback before it starts receiving process() calls.
     * @note Thread-safe - uses RCU for lock-free audio thread access.
     *
     * @warning NOT real-time safe - performs allocation.
     */
    void addCallback(AudioIODeviceCallback* callback);

    /**
     * @brief Unregisters an audio callback.
     *
     * Removes the callback from the list of registered callbacks. The
     * callback's releaseResources() method will be called if audio is currently
     * running.
     *
     * @param callback Pointer to the callback to remove.
     *
     * @note Safe to call with a callback that is not registered (no-op).
     * @note Thread-safe - uses RCU for lock-free audio thread access.
     *
     * @warning NOT real-time safe - performs allocation.
     */
    void removeCallback(AudioIODeviceCallback* callback);

    /**
     * @brief Sets a callback for device notification events.
     *
     * The notification callback is invoked when device events occur, such as:
     * - Device started (ma_device_notification_type_started)
     * - Device stopped (ma_device_notification_type_stopped)
     * - Device rerouted (ma_device_notification_type_rerouted)
     * - Device interruption began/ended
     *
     * @param callback The callback function to invoke on notifications,
     *                 or nullptr to disable notifications.
     *
     * @warning NOT real-time safe - modifies internal state.
     */
    void setDeviceNotificationCallback(DeviceNotificationCallback callback);

    /**
     * @brief Registers a logging callback with the miniaudio context log.
     *
     * This is post-init only. Logs emitted during context initialization
     * will not be captured.
     *
     * @param callback Log callback function (must not be null).
     * @param userData User data passed to the callback.
     *
     * @return true if the callback was registered, false otherwise.
     */
    bool registerLogCallback(ma_log_callback_proc callback, void* userData);

    /**
     * @brief Unregisters a logging callback from the miniaudio context log.
     *
     * @param callback Log callback function (must not be null).
     * @param userData User data passed to the callback.
     *
     * @return true if the callback was unregistered, false otherwise.
     */
    bool unregisterLogCallback(ma_log_callback_proc callback, void* userData);

    /**
     * @brief Gets the current sample rate.
     *
     * @return The actual device sample rate in Hz if initialised, otherwise
     *         the default (44100).
     */
    ma_uint32 getSampleRate() const {
        return m_deviceInitialised ? m_device.sampleRate : m_sampleRate;
    }

    /**
     * @brief Gets the current buffer size.
     *
     * @return The requested buffer size in frames, or the default (512) if not
     *         initialised.
     *
     * @note This is the requested buffer size; the actual size may differ
     *       depending on the audio driver.
     */
    ma_uint32 getBufferSize() const { return m_bufferSize; }

    /**
     * @brief Gets the current number of input (capture) channels.
     *
     * @return The actual capture channel count if initialised, otherwise the
     *         requested input channel count.
     */
    ma_uint32 getNumInputChannels() const {
        return m_deviceInitialised ? m_device.capture.channels
                                   : m_numInputChannels;
    }

    /**
     * @brief Gets the current number of output (playback) channels.
     *
     * @return The actual playback channel count if initialised, otherwise the
     *         requested output channel count.
     */
    ma_uint32 getNumOutputChannels() const {
        return m_deviceInitialised ? m_device.playback.channels
                                   : m_numOutputChannels;
    }

private:
    /// @brief Miniaudio context for device management
    ma_context m_context;
    /// @brief Miniaudio device handle
    ma_device m_device;
    /// @brief Whether the audio context is initialised
    bool m_contextInitialised = false;
    /// @brief Whether the audio device is initialised
    bool m_deviceInitialised = false;
    /// @brief Whether audio is currently running
    bool m_running = false;

    /// @brief Current sample rate in Hz
    ma_uint32 m_sampleRate = 44100;
    /// @brief Current buffer size in frames
    ma_uint32 m_bufferSize = 512;
    /// @brief Current number of input channels
    ma_uint32 m_numInputChannels = 1;
    /// @brief Current number of output channels
    ma_uint32 m_numOutputChannels = 1;

    /// @brief RCU-protected list of registered audio callbacks (lock-free reads)
    RCU<std::vector<AudioIODeviceCallback*>> m_callbacks;

    /// @brief Flag to ensure audio thread registers with RCU only once
    mutable std::atomic<bool> m_audioThreadRegistered{false};

    /// @brief User-provided device notification callback (atomic load/store)
    std::shared_ptr<DeviceNotificationCallback> m_notificationCallback;

    /**
     * @brief Static callback invoked by miniaudio for audio processing.
     *
     * @param pDevice Pointer to the miniaudio device
     * @param pOutput Pointer to the output buffer
     * @param pInput Pointer to the input buffer
     * @param frameCount Number of frames to process
     */
    static void dataCallback(ma_device* pDevice,
                             void* pOutput,
                             const void* pInput,
                             ma_uint32 frameCount);

    /**
     * @brief Static callback invoked by miniaudio for device notifications.
     *
     * @param pNotification Pointer to the notification data
     */
    static void notificationCallback(
        const ma_device_notification* pNotification);

    /**
     * @brief Dispatches audio data to all registered callbacks.
     *
     * Uses RCU for lock-free access to the callback list, ensuring no
     * blocking on the audio thread.
     *
     * @param output Pointer to the interleaved output buffer
     * @param input Pointer to the interleaved input buffer
     * @param frameCount Number of frames to process
     *
     * @note Real-time safe - uses lock-free RCU reads.
     */
    void processCallbacks(float* output,
                          const float* input,
                          ma_uint32 frameCount);

    /**
     * @brief Populates sample rate information for enumerated devices.
     *
     * @param devices Vector of devices to populate with sample rate data
     */
    void populateSampleRates(std::vector<AudioDeviceInfo>& devices) const;
};

}  // namespace thl
