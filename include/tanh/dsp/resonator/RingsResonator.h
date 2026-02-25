#pragma once

#include <cstdint>
#include <memory>

namespace thl::dsp::resonator {

/**
 * @brief Wrapper for Mutable Instruments Rings modal resonator
 *
 * The Rings module is a modal resonator that can simulate various
 * vibrating materials like strings, plates, and tubes.
 * Always uses external audio input as excitation.
 */
class RingsResonator {
public:
    /**
     * @brief Resonator model types
     */
    enum class ResonatorModel {
        Modal,                      // Modal resonator (default)
        SympatheticString,         // Sympathetic strings
        ModulatedString,           // Inharmonic string
        FMVoice,                   // 2-op FM voice
        SympatheticStringQuantized, // Sympathetic strings with quantized pitch
        StringAndReverb            // String with reverb (Disastrous Peace)
    };

    /**
     * @brief Polyphony modes
     */
    enum class PolyphonyMode {
        One,
        Two,
        Four
    };

    RingsResonator();
    ~RingsResonator();

    // Non-copyable, movable
    RingsResonator(const RingsResonator&) = delete;
    RingsResonator& operator=(const RingsResonator&) = delete;
    RingsResonator(RingsResonator&&) noexcept;
    RingsResonator& operator=(RingsResonator&&) noexcept;

    /**
     * @brief Prepare the resonator for processing
     * @param sampleRate Sample rate in Hz
     * @param maxBlockSize Maximum expected block size
     */
    void prepare(double sampleRate, int maxBlockSize);

    /**
     * @brief Process audio block
     *
     * Output is a blend of odd and even harmonics controlled by setOddEvenMix().
     *
     * @param input Input buffer (excitation signal)
     * @param output Output buffer (blended odd/even)
     * @param numSamples Number of samples to process
     */
    void process(const float* input, float* output, int numSamples);

    // Parameter setters (all values normalized 0.0 - 1.0 unless noted)

    /**
     * @brief Set the fundamental frequency
     * @param frequency Frequency in Hz (20 - 20000)
     */
    void setFrequency(float frequency);

    /**
     * @brief Set the structure parameter
     *
     * Controls the harmonic structure of the resonator.
     * Low values = harmonic series (strings, tubes)
     * High values = inharmonic (bells, plates)
     *
     * @param structure 0.0 - 1.0
     */
    void setStructure(float structure);

    /**
     * @brief Set the brightness parameter
     *
     * Controls high-frequency content / spectral tilt
     * Low values = dark, muted
     * High values = bright, present
     *
     * @param brightness 0.0 - 1.0
     */
    void setBrightness(float brightness);

    /**
     * @brief Set the damping parameter
     *
     * Controls the decay time of the resonator
     * Low values = long decay
     * High values = short decay
     *
     * @param damping 0.0 - 1.0
     */
    void setDamping(float damping);

    /**
     * @brief Set the position parameter
     *
     * Controls where on the resonator the excitation is applied.
     * Affects the balance of harmonics.
     * 0.0 = center (fundamental emphasis)
     * 1.0 = edge (harmonic emphasis)
     *
     * @param position 0.0 - 1.0
     */
    void setPosition(float position);

    /**
     * @brief Set the resonator model
     * @param model The resonator model to use
     */
    void setModel(ResonatorModel model);

    /**
     * @brief Set the polyphony mode
     * @param mode Number of voices (1, 2, or 4)
     */
    void setPolyphony(PolyphonyMode mode);

    /**
     * @brief Set the odd/even output blend
     *
     * Controls the mix between odd and even harmonic outputs.
     * 0.0 = odd harmonics only
     * 1.0 = even harmonics only
     *
     * @param mix 0.0 - 1.0
     */
    void setOddEvenMix(float mix);

    /**
     * @brief Set the dry/wet mix
     *
     * Global mix between raw (unresampled) input and processed output.
     * The dry signal is delayed to match the wet signal's latency.
     *
     * @param dryWet 0.0 = fully dry, 1.0 = fully wet
     */
    void setDryWet(float dryWet);

    /**
     * @brief Get the processing latency in host samples
     *
     * Returns the total pipeline latency including resampling and
     * block accumulation. Useful for latency compensation in DAWs.
     */
    int getLatency() const;

private:
    struct Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace thl::dsp::resonator
