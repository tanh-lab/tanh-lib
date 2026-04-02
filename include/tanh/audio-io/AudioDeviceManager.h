#pragma once
#include "AudioDeviceInfo.h"
#include "AudioIODeviceCallback.h"
#include <tanh/core/Logger.h>
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
 * @enum BluetoothProfile
 * @brief Bluetooth audio profile to use when a classic BT headset is connected.
 *
 * On iOS, the active Bluetooth profile determines audio quality and
 * microphone availability:
 *
 * | Profile | Sample Rate        | BT Mic Input |
 * |---------|--------------------|--------------|
 * | HFP     | 8 kHz / 16 kHz     | Yes          |
 * | A2DP    | 44.1 kHz / 48 kHz  | No (built-in mic fallback) |
 *
 * BLE Audio devices are handled automatically by the OS and do not
 * require profile switching.
 *
 * After switching you may want to re-initialise the audio device because
 * the sample rate and buffer configuration can change.
 *
 * @see AudioDeviceManager::setBluetoothProfile()
 */
enum class BluetoothProfile {
    A2DP,  ///< Advanced Audio Distribution Profile — output-only, high quality.
    HFP    ///< Hands-Free Profile — bidirectional, low quality.
};

/// Convert a BluetoothProfile enum value to its string representation.
const char* bluetooth_profile_to_string(BluetoothProfile profile);

/// Parse a string into a BluetoothProfile. Returns A2DP for unrecognised values.
BluetoothProfile bluetooth_profile_from_string(const std::string& str);

/// Returns the list of Bluetooth profiles supported on the current device/OS.
/// On Android API ≤ 28 only A2DP is returned.
std::vector<BluetoothProfile> get_supported_bluetooth_profiles();

/// Returns true if a classic Bluetooth device (A2DP / HFP / SCO) is connected.
/// BLE Audio devices are excluded — they don't need profile switching.
bool is_classic_bluetooth_connected();

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
     * @brief Identifies a device role for queries and internal routing.
     */
    enum class DeviceRole { Playback, Capture, Duplex };

    /**
     * @brief Callback type for device notification events.
     *
     * This callback is invoked when device events occur, such as the device
     * being started, stopped, or disconnected.
     *
     * @see setDeviceNotificationCallback()
     */
    using DeviceNotificationCallback = std::function<void(DeviceNotificationType)>;

    /**
     * @brief Callback type for log messages from the audio backend.
     *
     * @param level Normalised log level.
     * @param message The log message.
     */
    using LogCallback = std::function<void(Logger::LogLevel level, const char* message)>;

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

#if defined(THL_PLATFORM_ANDROID)
    /**
     * @brief Provides the JavaVM pointer required for Android device enumeration.
     *
     * Must be called once (e.g. from JNI_OnLoad) before enumerateInputDevices()
     * or enumerateOutputDevices() will return real device lists on Android.
     *
     * @param java_vm The JavaVM* obtained in JNI_OnLoad.
     */
    static void set_java_vm(void* java_vm);
#endif

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
    bool is_context_initialised() const;

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
    std::vector<AudioDeviceInfo> enumerate_input_devices() const;

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
    std::vector<AudioDeviceInfo> enumerate_output_devices() const;

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
    bool initialise(const AudioDeviceInfo* input_device,
                    const AudioDeviceInfo* output_device,
                    uint32_t sample_rate = 44100,
                    uint32_t buffer_size_in_frames = 512,
                    uint32_t num_input_channels = 1,
                    uint32_t num_output_channels = 1);

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
    bool start_playback();

    /**
     * @brief Stops playback audio processing.
     *
     * Halts audio output on the playback device. Playback callbacks will
     * receive release_resources() after audio processing stops.
     *
     * @warning NOT real-time safe - may block while waiting for audio thread to
     * stop.
     */
    void stop_playback();

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
    bool start_capture();

    /**
     * @brief Stops capture audio processing.
     *
     * Halts audio input on the capture device. Capture callbacks will
     * receive release_resources() after audio processing stops.
     *
     * @warning NOT real-time safe - may block while waiting for audio thread to
     * stop.
     */
    void stop_capture();

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
    bool start_duplex();

    /**
     * @brief Stops duplex audio processing.
     *
     * Halts audio input and output on the duplex device. Duplex callbacks will
     * receive release_resources() after audio processing stops.
     *
     * @warning NOT real-time safe - may block while waiting for audio thread to
     * stop.
     */
    void stop_duplex();

    /**
     * @brief Checks if playback processing is currently running.
     *
     * @return true if playback is actively processing audio, false otherwise.
     */
    bool is_playback_running() const { return m_playback_running.load(std::memory_order_relaxed); }

    /**
     * @brief Checks if capture processing is currently running.
     *
     * @return true if capture is actively processing audio, false otherwise.
     */
    bool is_capture_running() const { return m_capture_running.load(std::memory_order_relaxed); }

    /**
     * @brief Checks if duplex processing is currently running.
     *
     * @return true if duplex is actively processing audio, false otherwise.
     */
    bool is_duplex_running() const { return m_duplex_running.load(std::memory_order_relaxed); }

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
     * @note If the playback device is already initialised, prepare_to_play()
     *       will be called on the callback before it starts receiving
     *       process() calls.
     * @note Thread-safe - uses RCU for lock-free audio thread access.
     *
     * @warning NOT real-time safe - performs allocation.
     */
    void add_playback_callback(AudioIODeviceCallback* callback);

    /**
     * @brief Registers a capture audio callback to receive audio data.
     *
     * The callback's process() method will be called on the capture audio
     * thread for each buffer of audio data.
     *
     * @param callback Pointer to the callback to register. Must remain valid
     *                 until removed or the manager is destroyed.
     *
     * @note If the capture device is already initialised, prepare_to_play()
     *       will be called on the callback before it starts receiving
     *       process() calls.
     * @note Thread-safe - uses RCU for lock-free audio thread access.
     *
     * @warning NOT real-time safe - performs allocation.
     */
    void add_capture_callback(AudioIODeviceCallback* callback);

    /**
     * @brief Registers a duplex audio callback to receive audio data.
     *
     * The callback's process() method will be called on the duplex audio
     * thread for each buffer of audio data.
     *
     * @param callback Pointer to the callback to register. Must remain valid
     *                 until removed or the manager is destroyed.
     *
     * @note If the duplex device is already initialised, prepare_to_play()
     *       will be called on the callback before it starts receiving
     *       process() calls.
     * @note Thread-safe - uses RCU for lock-free audio thread access.
     *
     * @warning NOT real-time safe - performs allocation.
     */
    void add_duplex_callback(AudioIODeviceCallback* callback);

    /**
     * @brief Unregisters a playback audio callback.
     *
     * Removes the callback from the list of registered callbacks. The
     * callback's release_resources() method will be called if playback is
     * currently running.
     *
     * @param callback Pointer to the callback to remove.
     *
     * @note Safe to call with a callback that is not registered (no-op).
     * @note Thread-safe - uses RCU for lock-free audio thread access.
     *
     * @warning NOT real-time safe - performs allocation.
     */
    void remove_playback_callback(AudioIODeviceCallback* callback);

    /**
     * @brief Unregisters a capture audio callback.
     *
     * Removes the callback from the list of registered callbacks. The
     * callback's release_resources() method will be called if capture is
     * currently running.
     *
     * @param callback Pointer to the callback to remove.
     *
     * @note Safe to call with a callback that is not registered (no-op).
     * @note Thread-safe - uses RCU for lock-free audio thread access.
     *
     * @warning NOT real-time safe - performs allocation.
     */
    void remove_capture_callback(AudioIODeviceCallback* callback);

    /**
     * @brief Unregisters a duplex audio callback.
     *
     * Removes the callback from the list of registered callbacks. The
     * callback's release_resources() method will be called if duplex is
     * currently running.
     *
     * @param callback Pointer to the callback to remove.
     *
     * @note Safe to call with a callback that is not registered (no-op).
     * @note Thread-safe - uses RCU for lock-free audio thread access.
     *
     * @warning NOT real-time safe - performs allocation.
     */
    void remove_duplex_callback(AudioIODeviceCallback* callback);

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
    void set_device_notification_callback(DeviceNotificationCallback callback);

    /**
     * @brief Sets a callback for log messages from the audio backend.
     *
     * This is post-init only. Logs emitted during context initialization
     * will not be captured.
     *
     * @param callback Log callback function, or nullptr to clear.
     */
    void set_log_callback(LogCallback callback);

    /**
     * @brief Switches the iOS Bluetooth audio profile.
     *
     * Reconfigures the AVAudioSession category options to use the specified
     * Bluetooth profile. After calling this you must shutdown() and
     * re-initialise() the device because the sample rate and buffer
     * configuration will have changed.
     *
     * On non-iOS platforms this is a no-op that returns true.
     *
     * @param profile The desired Bluetooth profile.
     * @return true if the session was reconfigured successfully.
     *
     * @warning Must be called while no devices are running.
     * @warning NOT real-time safe - performs system calls.
     *
     * @see BluetoothProfile
     */
    bool set_bluetooth_profile(BluetoothProfile profile);

    /**
     * @brief Returns the currently configured Bluetooth profile.
     *
     * @return The active BluetoothProfile (defaults to HFP).
     */
    BluetoothProfile get_bluetooth_profile() const;

    /**
     * @brief Gets the current sample rate.
     *
     * @return The actual device sample rate in Hz if initialised, otherwise
     *         the default (44100).
     */
    uint32_t get_sample_rate() const;

    /**
     * @brief Gets the actual capture device sample rate.
     *
     * On Android with Bluetooth SCO active, AAudio may report an incorrect
     * sample rate for the capture stream.  This method uses a callback-based
     * measurement of the actual frame delivery rate when available.
     * In all other cases it returns getSampleRate().
     *
     * Use this when opening a recording file so the WAV header matches
     * the actual audio data rate.
     *
     * @return The true capture sample rate in Hz.
     */
    uint32_t get_capture_sample_rate() const;

    /**
     * @brief Blocks until the capture rate measurement has completed or
     *        the timeout expires.
     *
     * On Android with Bluetooth SCO, the actual sample rate is measured by
     * timing capture callbacks.  Call this before getCaptureSampleRate()
     * when accuracy is critical (e.g. before writing a WAV header).
     *
     * If capture is not running or SCO is not active, returns immediately.
     *
     * @param timeoutMs Maximum time to wait in milliseconds (default 2000).
     * @return true if a measurement is available, false on timeout.
     */
    bool wait_for_capture_rate_measurement(uint32_t timeout_ms = 2000) const;

    /**
     * @brief Gets the current buffer size.
     *
     * Returns the resolved period size in frames used for callback preparation.
     * Without a role argument, returns the value using priority order:
     * playback > duplex > capture. With a role argument, returns the value
     * for that specific role.
     *
     * @param role Optional device role to query. If omitted, uses priority order.
     * @return The resolved period size in frames, or 512 if no device is
     *         initialised.
     *
     * @note The returned value may differ from the requested buffer size
     *       depending on the audio driver.
     */
    uint32_t get_buffer_size(DeviceRole role) const;
    uint32_t get_buffer_size() const;

    /**
     * @brief Gets the per-callback period (chunk) size in frames.
     *
     * Returns the actual period size observed from the audio callback.
     * On the first callback, the actual frame count is recorded and used
     * for subsequent queries — this may differ from the prepared period size
     * (e.g. on iOS where the Audio Unit may deliver fewer frames than
     * AVAudioSession reports).
     *
     * Without a role argument, returns the value using priority order:
     * playback > duplex > capture. With a role argument, returns the value
     * for that specific role.
     *
     * @param role Optional device role to query. If omitted, uses priority order.
     * @return The period size used for each audio callback, or the buffer size
     *         default if not initialised.
     *
     * @note Updated on the first audio callback to reflect the true frame count.
     */
    uint32_t get_period_size(DeviceRole role) const;
    uint32_t get_period_size() const;

    /**
     * @brief Gets the number of periods that make up the total buffer.
     *
     * On Android this is bufferSize / burstSize; on Apple platforms it is 1.
     * Without a role argument, returns the value using priority order:
     * playback > duplex > capture.
     *
     * @param role Optional device role to query. If omitted, uses priority order.
     * @return The period count, or 1 if not initialised.
     */
    uint32_t get_period_count(DeviceRole role) const;
    uint32_t get_period_count() const;

    /**
     * @brief Gets the hardware burst size in frames.
     *
     * On Android this is the AAudio framesPerBurst (the smallest callback
     * granularity). On Apple platforms it equals the period size.
     * Use this as the base unit for generating selectable buffer sizes.
     *
     * Without a role argument, returns the value using priority order:
     * playback > duplex > capture. With a role argument, returns the value
     * for that specific role. Falls back to the period size if unavailable.
     *
     * @param role Optional device role to query. If omitted, uses priority order.
     * @return The hardware burst size, or the period size if unavailable.
     */
    uint32_t get_burst_size(DeviceRole role) const;
    uint32_t get_burst_size() const;

    /**
     * @brief Gets the current number of input (capture) channels.
     *
     * @return The actual capture channel count if initialised, otherwise the
     *         requested input channel count.
     */
    uint32_t get_num_input_channels() const;

    /**
     * @brief Gets the current number of output (playback) channels.
     *
     * @return The actual playback channel count if initialised, otherwise the
     *         requested output channel count.
     */
    uint32_t get_num_output_channels() const;

    /**
     * @brief Gets the name of the current output device.
     *
     * On iOS returns the active AVAudioSession output route name.
     * On other platforms returns the device name passed to initialise().
     *
     * @return Device name string, or empty if not initialised.
     */
    std::string get_current_output_device_name() const;

    /**
     * @brief Gets the name of the current input device.
     *
     * On iOS returns the active AVAudioSession input route name.
     * On other platforms returns the device name passed to initialise().
     *
     * @return Device name string, or empty if not initialised.
     */
    std::string get_current_input_device_name() const;

    /**
     * @brief Maximum IO buffer duration (in seconds) safe for Bluetooth HFP.
     *
     * Bluetooth SCO links used by HFP can fail silently when the iOS
     * AVAudioSession preferred IO buffer duration exceeds this threshold.
     */
    static constexpr float k_max_bluetooth_io_buffer_duration_seconds = 0.064f;

    /**
     * @brief Clamps a buffer size so the resulting IO buffer duration stays
     *        within the Bluetooth HFP safe limit.
     *
     * If the buffer size would produce an IO buffer duration longer than
     * kMaxBluetoothIOBufferDurationSeconds, the returned size is reduced to
     * exactly that limit.
     *
     * @param bufferSizeInFrames The requested buffer size in frames.
     * @param sampleRate The sample rate in Hz.
     * @return The (possibly reduced) buffer size in frames.
     */
    static uint32_t clamp_buffer_size_for_bluetooth_route(uint32_t buffer_size_in_frames,
                                                          uint32_t sample_rate);

private:
    struct Impl;
    struct DeviceUserData;

    std::unique_ptr<Impl> m_impl;

    std::vector<AudioDeviceInfo> enumerate_devices(DeviceType type) const;
    void populate_sample_rates(std::vector<AudioDeviceInfo>& devices) const;

    bool try_initialise_device(DeviceRole role,
                               const AudioDeviceInfo* input_device,
                               const AudioDeviceInfo* output_device,
                               uint32_t sample_rate,
                               uint32_t buffer_size_in_frames,
                               uint32_t num_input_channels,
                               uint32_t num_output_channels);

    void process_callbacks(DeviceRole role,
                           void* device,
                           float* output,
                           const float* input,
                           uint32_t frame_count);

    static void data_callback(void* p_device,
                              void* p_output,
                              const void* p_input,
                              uint32_t frame_count);

    static void notification_callback(const void* p_notification);

    static void static_log_callback(void* p_user_data, uint32_t level, const char* p_message);

    std::atomic<bool> m_playback_running{false};
    std::atomic<bool> m_capture_running{false};
    std::atomic<bool> m_duplex_running{false};

    uint32_t m_sample_rate = 44100;
    uint32_t m_num_input_channels = 1;
    uint32_t m_num_output_channels = 1;
    BluetoothProfile m_bluetooth_profile = BluetoothProfile::A2DP;

    RCU<std::vector<AudioIODeviceCallback*>> m_playback_callbacks;
    RCU<std::vector<AudioIODeviceCallback*>> m_capture_callbacks;
    RCU<std::vector<AudioIODeviceCallback*>> m_duplex_callbacks;

    mutable std::atomic<bool> m_playback_audio_thread_registered{false};
    mutable std::atomic<bool> m_capture_audio_thread_registered{false};
    mutable std::atomic<bool> m_duplex_audio_thread_registered{false};

    std::shared_ptr<DeviceNotificationCallback> m_notification_callback;
};

}  // namespace thl
