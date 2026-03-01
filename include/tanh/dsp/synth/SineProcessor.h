#pragma once

#include <cmath>
#include <vector>
#include "tanh/dsp/BaseProcessor.h"
#include "tanh/dsp/utils/SmoothedValue.h"

namespace thl {
namespace dsp {
namespace synth {

class SineProcessorImpl : public BaseProcessor {
public:
    SineProcessorImpl();
    ~SineProcessorImpl() override;

    // BaseProcessor overrides
    void prepare(const double& sample_rate, const size_t& samples_per_block, const size_t& num_channels) override;
    void process(float** buffer, const size_t& num_samples, const size_t& num_channels) override;

protected:
    enum Parameter {
        Frequency = 0,
        Amplitude = 1,

        NUM_PARAMETERS = 2
    };
    
private:
    // Template wrapper for get_parameter
    template<typename T>
    T get_parameter(Parameter parameter);

    virtual float get_parameter_float(Parameter parameter) = 0;

    std::vector<float> m_phase;

    utils::SmoothedValue smoothed_frequency;
    utils::SmoothedValue smoothed_amplitude;

    double m_sample_rate = 44100.f;
    size_t m_samples_per_block = 512;
};

// Template specializations for get_parameter
template<> inline float SineProcessorImpl::get_parameter<float>(Parameter p) { return get_parameter_float(p); }

} // namespace synth
} // namespace dsp
} // namespace thl
