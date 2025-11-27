#pragma once

namespace thl {
namespace dsp {

class BaseProcessor {
public:
    virtual ~BaseProcessor() = default;

    virtual void prepare(const double& sample_rate, const size_t& samples_per_block, const size_t& num_channels) = 0;
    virtual void process(float** buffer, const size_t& num_samples, const size_t& num_channels) = 0;

    virtual void set_parameter(int param_id, float value) = 0;
    virtual float get_parameter(int param_id) const = 0;
};

} // namespace dsp
} // namespace thl
