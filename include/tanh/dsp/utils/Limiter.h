#pragma once

#include <cstddef>
#include <tanh/dsp/BaseProcessor.h>

namespace thl::dsp::utils {

class LimiterImpl : public BaseProcessor {
public:
    LimiterImpl();
    ~LimiterImpl() override;

    void prepare(const double& sample_rate, const size_t& samples_per_block, const size_t& num_channels) override;
    void process(float** buffer, const size_t& num_samples, const size_t& num_channels) override;

protected:
    enum Parameter {
        Threshold = 0,
        Attack = 1,
        Release = 2,

        NUM_PARAMETERS = 3
    };

    template<typename T>
    T get_parameter(Parameter parameter);

    virtual float get_parameter_float(Parameter parameter) = 0;
    virtual bool get_parameter_bool(Parameter parameter) = 0;
    virtual int get_parameter_int(Parameter parameter) = 0;

private:
    double m_sample_rate = 48000.0;
    size_t m_channels = 2;

    float m_gain = 1.0f;
    float m_attack_coeff = 0.0f;
    float m_release_coeff = 0.0f;
};

template<> inline float LimiterImpl::get_parameter<float>(Parameter p) { return get_parameter_float(p); }
template<> inline bool LimiterImpl::get_parameter<bool>(Parameter p) { return get_parameter_bool(p); }
template<> inline int LimiterImpl::get_parameter<int>(Parameter p) { return get_parameter_int(p); }

} // namespace thl::dsp::utils
