#pragma once

#include <tanh/core/Exports.h>
#include <tanh/dsp/BaseProcessor.h>
#include <tanh/dsp/utils/SmoothedValue.h>

#include <array>
#include <vector>

namespace thl::dsp::fx {

/**
 * Intellijel-style wavefolder processor.
 *
 * Models the analog wavefolder topology found in the Intellijel µFold /
 * Bifold modules. Uses a sine-shaping transfer function with a continuous
 * `Folds` parameter, followed by a 12 dB/oct lowpass `Tone` control that
 * darkens the folded harmonics.
 *
 * Folding is level-independent. A per-channel peak follower (instant attack,
 * 50 ms release) tracks the input envelope; the signal is divided by it, folded
 * in that unit domain and scaled back by it. The same Drive therefore produces
 * the same harmonics on a quiet and a loud input, and the output follows the
 * input's envelope. Input below -60 dBFS is treated as silence, so a silent
 * input stays silent at any Symmetry.
 *
 * The output level is set by a fixed makeup gain that depends only on the
 * (smoothed) Drive·(1 + Folds): unity gain while the sine is still
 * linear, unity peak once it folds. The shaper's zero-input value is
 * subtracted, so Symmetry adds even harmonics without adding DC that follows
 * the envelope; a one-pole DC blocker removes what remains.
 *
 * All parameters are smoothed per sample, so they can be modulated or stepped
 * at control rate without zipper noise.
 *
 * Processes all input channels in-place.
 *
 * Parameters:
 *   Drive     – gain into the folder relative to the input envelope [0.1, 20];
 *               the first fold starts at Drive·(1 + Folds) = π/2
 *   Folds     – fold depth [0, 10]; fractional values interpolate smoothly
 *   Symmetry  – offset before folding, relative to the envelope [-1, 1];
 *               adds even harmonics
 *   Tone      – lowpass cutoff [0 = dark (1 kHz), 1 = open (just below Nyquist)],
 *               exponential sweep
 */
class TANH_API IntellijelWavefolderImpl : public thl::dsp::BaseProcessor {
public:
    IntellijelWavefolderImpl();
    ~IntellijelWavefolderImpl() override;

    void prepare(const double& sample_rate,
                 const size_t& samples_per_block,
                 const size_t& num_channels) override;

    void process(thl::core::BufferView buffer, uint32_t modulation_offset = 0) override;

protected:
    enum Parameter { Drive = 0, Folds, Symmetry, Tone, NumParameters };

    template <typename T>
    T get_parameter(Parameter p, uint32_t modulation_offset = 0);

    virtual float get_parameter_float(Parameter p, uint32_t modulation_offset = 0) = 0;

private:
    static float shape(float u, float k, float symmetry);
    [[nodiscard]] float tone_coefficient(float tone) const;

    utils::SmoothedValue m_smoothed_drive;
    utils::SmoothedValue m_smoothed_folds;
    utils::SmoothedValue m_smoothed_symmetry;
    utils::SmoothedValue m_smoothed_tone;

    // One-pole DC blocker — y[n] = x[n] - x[n-1] + pole * y[n-1].
    std::vector<float> m_dc_x_prev;
    std::vector<float> m_dc_y_prev;
    float m_dc_pole = 0.0f;

    // Per-channel input peak follower. The folder works on input / envelope
    // and scales the result back by it.
    std::vector<float> m_envelope;

    // Per-channel state of the two Tone lowpass stages.
    std::vector<std::array<float, 2>> m_tone_state;
    float m_sample_rate = 48000.0f;
    float m_env_release = 0.0f;
};

template <>
inline float IntellijelWavefolderImpl::get_parameter<float>(Parameter p,
                                                            uint32_t modulation_offset) {
    return get_parameter_float(p, modulation_offset);
}

}  // namespace thl::dsp::fx
