#pragma once

#include <cstddef>
#include <tanh/dsp/BaseProcessor.h>

namespace thl::dsp::utils {

class LimiterImpl : public BaseProcessor {
public:
    LimiterImpl();
    ~LimiterImpl() override;

    void prepare(const double& sample_rate,
                 const size_t& samples_per_block,
                 const size_t& num_channels) override;
    void process(thl::dsp::audio::AudioBufferView buffer,
                 uint32_t modulation_offset = 0) override;

protected:
    enum Parameter {
        Threshold = 0,
        Attack = 1,
        Release = 2,

        NUM_PARAMETERS = 3
    };

    template <typename T>
    T get_parameter(Parameter parameter, uint32_t modulation_offset = 0);

    virtual float get_parameter_float(Parameter parameter, uint32_t modulation_offset = 0) = 0;
    virtual bool get_parameter_bool(Parameter parameter, uint32_t modulation_offset = 0) = 0;
    virtual int get_parameter_int(Parameter parameter, uint32_t modulation_offset = 0) = 0;

private:
    double m_sample_rate = 48000.0;
    size_t m_channels = 2;

    float m_gain = 1.0f;
    float m_attack_coeff = 0.0f;
    float m_release_coeff = 0.0f;
};

template <>
inline float LimiterImpl::get_parameter<float>(Parameter p,
                                               uint32_t modulation_offset) {
    return get_parameter_float(p, modulation_offset);
}
template <>
inline bool LimiterImpl::get_parameter<bool>(Parameter p,
                                              uint32_t modulation_offset) {
    return get_parameter_bool(p, modulation_offset);
}
template <>
inline int LimiterImpl::get_parameter<int>(Parameter p,
                                            uint32_t modulation_offset) {
    return get_parameter_int(p, modulation_offset);
}

}  // namespace thl::dsp::utils
