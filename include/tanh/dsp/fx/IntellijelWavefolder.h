#pragma once

#include <vector>

#include <tanh/core/Exports.h>
#include <tanh/dsp/BaseProcessor.h>
#include <tanh/dsp/utils/SmoothedValue.h>

namespace thl::dsp::fx {

/**
 * Intellijel-style wavefolder processor.
 *
 * Models the analog wavefolder topology found in the Intellijel µFold /
 * Bifold modules. Uses a sine-shaping transfer function with a continuous
 * `Folds` parameter, combined with an optional JFET-like soft saturation
 * stage for tonal warmth.
 *
 * Per-sample parameter smoothing prevents zipper noise when Drive/Folds/
 * Symmetry/JfetTone are modulated at control rate. A one-pole DC blocker on
 * the output removes the static DC offset the transfer function produces for
 * non-zero Symmetry.
 *
 * Processes all input channels in-place.
 *
 * Parameters:
 *   Drive     – input gain before folding [0.1, 20]
 *   Folds     – fold depth [0, 10]; fractional values interpolate smoothly
 *   Symmetry  – DC offset before folding [-1, 1]; adds even harmonics
 *   JfetTone  – blend of JFET soft saturation [0 = clean, 1 = full]
 */
class TANH_API IntellijelWavefolderImpl : public thl::dsp::BaseProcessor {
public:
    IntellijelWavefolderImpl();
    ~IntellijelWavefolderImpl() override;

    void prepare(const double& sample_rate,
                 const size_t& samples_per_block,
                 const size_t& num_channels) override;

    void process(thl::dsp::audio::AudioBufferView buffer, uint32_t modulation_offset = 0) override;

protected:
    enum Parameter { Drive = 0, Folds, Symmetry, JfetTone, NumParameters };

    template <typename T>
    T get_parameter(Parameter p, uint32_t modulation_offset = 0);

    virtual float get_parameter_float(Parameter p, uint32_t modulation_offset = 0) = 0;

private:
    static float process_sample(float x, float drive, float folds, float symmetry, float jfet_tone);
    static float jfet_saturate(float x);

    utils::SmoothedValue m_smoothed_drive;
    utils::SmoothedValue m_smoothed_folds;
    utils::SmoothedValue m_smoothed_symmetry;
    utils::SmoothedValue m_smoothed_jfet_tone;

    // One-pole DC blocker — y[n] = x[n] - x[n-1] + pole * y[n-1].
    std::vector<float> m_dc_x_prev;
    std::vector<float> m_dc_y_prev;
    float m_dc_pole = 0.0f;
};

template <>
inline float IntellijelWavefolderImpl::get_parameter<float>(Parameter p,
                                                            uint32_t modulation_offset) {
    return get_parameter_float(p, modulation_offset);
}

}  // namespace thl::dsp::fx
