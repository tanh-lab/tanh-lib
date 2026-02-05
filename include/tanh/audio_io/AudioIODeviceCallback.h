#pragma once
#include "miniaudio.h"

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
 * The prepareToPlay() and releaseResources() methods are called from the main
 * thread and may perform allocations and other non-real-time-safe operations.
 *
 * @section usage Usage
 *
 * To use this interface:
 * 1. Derive from AudioIODeviceCallback and implement process()
 * 2. Optionally override prepareToPlay() for initialisation
 * 3. Optionally override releaseResources() for cleanup
 * 4. Register the callback with AudioDeviceManager::addCallback()
 *
 * @code
 * class MyProcessor : public AudioIODeviceCallback {
 * public:
 *     void process(float* output, const float* input,
 *                  ma_uint32 frameCount,
 *                  ma_uint32 numInputChannels,
 *                  ma_uint32 numOutputChannels) override {
 *         // Process audio here (real-time safe!)
 *         if (!output || !input) return;
 *         for (ma_uint32 i = 0; i < frameCount * numOutputChannels; ++i) {
 *             output[i] = input[i] * 0.5f;  // Simple gain reduction
 *         }
 *     }
 * };
 * @endcode
 *
 * @see AudioDeviceManager::addCallback()
 * @see AudioDeviceManager::removeCallback()
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
    virtual void process(float* outputBuffer,
                         const float* inputBuffer,
                         ma_uint32 frameCount,
                         ma_uint32 numInputChannels,
                         ma_uint32 numOutputChannels) = 0;

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
    virtual void prepareToPlay(ma_uint32 sampleRate, ma_uint32 bufferSize) {
        (void)sampleRate;
        (void)bufferSize;
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
    virtual void releaseResources() {}
};

}  // namespace thl
