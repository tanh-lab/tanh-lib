#pragma once

#include <cmath>
#include <vector>
#include "tanh/core/Exports.h"
#include "tanh/dsp/BaseProcessor.h"
#include "tanh/dsp/utils/SmoothedValue.h"



namespace thl::dsp::synth {

class TANH_API SineProcessorImpl : public BaseProcessor {
public:
    SineProcessorImpl();
    ~SineProcessorImpl() override;

    // BaseProcessor overrides
    void prepare(const double& sample_rate,
                 const size_t& samples_per_block,
                 const size_t& num_channels) override;
    void process(thl::dsp::audio::AudioBufferView buffer, uint32_t modulation_offset = 0) override;

protected:
    enum Parameter {
        Frequency = 0,
        Amplitude = 1,

        NumParameters = 2
    };

private:
    // Template wrapper for get_parameter
    template <typename T>
    T get_parameter(Parameter parameter, uint32_t modulation_offset = 0);

    virtual float get_parameter_float(Parameter parameter, uint32_t modulation_offset = 0) = 0;

    std::vector<float> m_phase;

    utils::SmoothedValue m_smoothed_frequency;
    utils::SmoothedValue m_smoothed_amplitude;

    double m_sample_rate = 44100.f;
    size_t m_samples_per_block = 512;
};

// Template specializations for get_parameter
template <>
inline float SineProcessorImpl::get_parameter<float>(Parameter p, uint32_t modulation_offset) {
    return get_parameter_float(p, modulation_offset);
}

} // namespace thl::dsp::synth


