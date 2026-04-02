#pragma once
#include <cstdint>

namespace thl {

/**
 * @class AudioIODeviceCallback
 * @brief Abstract interface for receiving audio I/O callbacks from an
 * AudioDeviceManager.
 *
 * AudioIODeviceCallback defines the interface for objects that process audio
 * data. Implementations receive audio input buffers and produce audio output
 * through the process() method, which is called from the audio thread.
 *
 * @section rt_safety Real-Time Safety
 *
 * The process() method is called from the audio thread and must be real-time
 * safe:
 * - No memory allocation
 * - No blocking operations (locks, I/O, system calls)
 * - No unbounded loops
 * - Deterministic execution time
 *
 * The prepare_to_play() and release_resources() methods are called from the main
 * thread and may perform allocations and other non-real-time-safe operations.
 *
 * @section usage Usage
 *
 * To use this interface:
 * 1. Derive from AudioIODeviceCallback and implement process()
 * 2. Optionally override prepare_to_play() for initialisation
 * 3. Optionally override release_resources() for cleanup
 * 4. Register the callback with AudioDeviceManager::addPlaybackCallback(),
 *    addCaptureCallback(), or addDuplexCallback()
 *
 * @code
 * class MyProcessor : public AudioIODeviceCallback {
 * public:
 *     void process(float* output, const float* input,
 *                  uint32_t frameCount,
 *                  uint32_t numInputChannels,
 *                  uint32_t numOutputChannels) override {
 *         // Process audio here (real-time safe!)
 *         if (!output || !input) return;
 *         for (uint32_t i = 0; i < frameCount * numOutputChannels; ++i) {
 *             output[i] = input[i] * 0.5f;  // Simple gain reduction
 *         }
 *     }
 * };
 * @endcode
 *
 * @see AudioDeviceManager::addPlaybackCallback()
 * @see AudioDeviceManager::removePlaybackCallback()
 * @see AudioDeviceManager::addCaptureCallback()
 * @see AudioDeviceManager::removeCaptureCallback()
 * @see AudioDeviceManager::addDuplexCallback()
 * @see AudioDeviceManager::removeDuplexCallback()
 */
class AudioIODeviceCallback {
public:
    /**
     * @brief Virtual destructor for proper cleanup of derived classes.
     */
    virtual ~AudioIODeviceCallback() = default;

    /**
     * @brief Processes audio data from input to output buffers.
     *
     * This method is called repeatedly from the audio thread to process audio.
     * The output buffer should be filled with audio data; it may contain
     * uninitialised data on entry.
     *
     * @param outputBuffer Pointer to the interleaved output buffer to fill.
     *                     Size is frameCount * numOutputChannels floats. May
     *                     be nullptr if no output device is active.
     * @param inputBuffer Pointer to the interleaved input buffer containing
     *                    captured audio. Size is frameCount * numInputChannels
     *                    floats. May be nullptr if no input device is active.
     * @param frameCount Number of audio frames to process. Each frame contains
     *                   numInputChannels/numOutputChannels samples.
     * @param numInputChannels Number of input channels (interleaved).
     * @param numOutputChannels Number of output channels (interleaved).
     *
     * @warning **MUST BE REAL-TIME SAFE** - Called from the audio thread.
     *          No allocations, locks, or blocking operations allowed.
     */
    virtual void process(float* output_buffer,
                         const float* input_buffer,
                         uint32_t frame_count,
                         uint32_t num_input_channels,
                         uint32_t num_output_channels) = 0;

    /**
     * @brief Called before audio processing begins to allow resource
     * preparation.
     *
     * Override this method to allocate buffers, initialise DSP state, or
     * perform other setup that depends on the audio configuration. This is
     * called from the main thread when the audio device is started.
     *
     * @param sampleRate The sample rate at which audio will be processed (e.g.,
     * 44100, 48000).
     * @param bufferSize The number of frames that will be passed to each
     * process() call.
     *
     * @note Called from the main thread - may perform allocations and blocking
     * operations.
     * @note Default implementation does nothing.
     */
    virtual void prepare_to_play(uint32_t sample_rate, uint32_t buffer_size) {
        (void)sample_rate;
        (void)buffer_size;
    }

    /**
     * @brief Called when audio processing stops to allow resource cleanup.
     *
     * Override this method to release buffers, reset DSP state, or perform
     * other cleanup. This is called from the main thread when the audio device
     * is stopped or when this callback is removed from the device manager.
     *
     * @note Called from the main thread - may perform deallocations and
     * blocking operations.
     * @note Default implementation does nothing.
     */
    virtual void release_resources() {}
};

}  // namespace thl
