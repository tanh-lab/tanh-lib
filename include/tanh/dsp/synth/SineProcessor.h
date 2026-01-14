#pragma once

#include <cmath>
#include <vector>
#include "tanh/dsp/BaseProcessor.h"
#include "tanh/dsp/utils/SmoothedValue.h"

namespace thl {
namespace dsp {
namespace synth {

class SineProcessor : public BaseProcessor {
public:
    enum Parameters {
        FREQUENCY = 0,
        AMPLITUDE = 1
    };

    SineProcessor();
    ~SineProcessor() override;

    // BaseProcessor overrides
    void prepare(const double& sample_rate, const size_t& samples_per_block, const size_t& num_channels) override;
    void process(float** buffer, const size_t& num_samples, const size_t& num_channels) override;

    void set_parameter(Parameters param, float value);

    float get_parameter(Parameters param) const;

private:
    void set_parameter(int param_id, float value) override;
    float get_parameter(int param_id) const override;

    std::vector<float> m_phase;

    float m_frequency = 100.f;
    float m_amplitude = 0.5f;

    utils::SmoothedValue smoothed_frequency;
    utils::SmoothedValue smoothed_amplitude;

    double m_sample_rate = 44100.f;
    size_t m_samples_per_block = 512;
};

} // namespace synth
} // namespace dsp
} // namespace thl
