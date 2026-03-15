#pragma once

#include <tanh/dsp/BaseProcessor.h>

namespace thl::dsp::fx {

/**
 * Intellijel-style wavefolder processor.
 *
 * Models the analog wavefolder topology found in the Intellijel µFold /
 * Bifold modules. Uses a sine-shaping transfer function with a continuous
 * `Folds` parameter, combined with an optional JFET-like soft saturation
 * stage for tonal warmth. Stateless — no per-sample history.
 *
 * Processes all input channels in-place.
 *
 * Parameters:
 *   Drive     – input gain before folding [0.1, 20]
 *   Folds     – fold depth [0, 10]; fractional values interpolate smoothly
 *   Symmetry  – DC offset before folding [-1, 1]; adds even harmonics
 *   JfetTone  – blend of JFET soft saturation [0 = clean, 1 = full]
 */
class IntellijelWavefolderImpl : public thl::dsp::BaseProcessor {
public:
    IntellijelWavefolderImpl();
    ~IntellijelWavefolderImpl() override;

    void prepare(const double& sample_rate,
                 const size_t& samples_per_block,
                 const size_t& num_channels) override;

    void process(thl::dsp::audio::AudioBufferView buffer) override;

protected:
    enum Parameter {
        Drive = 0,
        Folds,
        Symmetry,
        JfetTone,
        NUM_PARAMETERS
    };

    template <typename T>
    T get_parameter(Parameter p);

    virtual float get_parameter_float(Parameter p) = 0;

private:
    static float process_sample(float x, float drive, float folds,
                                float symmetry, float jfet_tone);
    static float jfet_saturate(float x);
};

template <>
inline float IntellijelWavefolderImpl::get_parameter<float>(Parameter p) {
    return get_parameter_float(p);
}

}  // namespace thl::dsp::fx
