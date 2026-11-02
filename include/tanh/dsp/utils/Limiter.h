#pragma once

#include <tanh/core/AbiNamespace.h>
#include <tanh/core/Exports.h>
#include <tanh/dsp/BaseProcessor.h>
#include <tanh/dsp/utils/SmoothedValue.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <cstddef>
#include <cstdint>

namespace thl::inline THL_ABI_NAMESPACE::dsp::utils {

class TANH_API LimiterImpl : public BaseProcessor {
public:
    LimiterImpl();
    ~LimiterImpl() override;

    void prepare(const double& sample_rate,
                 const size_t& samples_per_block,
                 const size_t& num_channels) override;
    void process(thl::core::BufferView buffer,
                 uint32_t modulation_offset = 0) TANH_NONBLOCKING_FUNCTION override;
    /// Releases the gain reduction at once.
    void reset() TANH_NONBLOCKING_FUNCTION override { m_gain = 1.0f; }

protected:
    enum Parameter {
        Threshold = 0,
        Attack = 1,
        Release = 2,

        NumParameters = 3
    };

    template <typename T>
    T get_parameter(Parameter parameter, uint32_t modulation_offset = 0);

    /// A NaN keeps the parameter's last value; the threshold is clamped to [-120, 24] dB, the
    /// times to [0.01, 60000] ms.
    virtual float get_parameter_float(Parameter parameter, uint32_t modulation_offset = 0) = 0;

private:
    void read_parameters(uint32_t modulation_offset) TANH_NONBLOCKING_FUNCTION;

    double m_sample_rate = 48000.0;
    // As read last (non-NaN): a NaN parameter keeps these.
    float m_threshold_db = 0.0f;
    float m_attack_ms = 1.0f;
    float m_release_ms = 100.0f;

    float m_gain = 1.0f;

    SmoothedValue m_smoothed_threshold;
    SmoothedValue m_smoothed_attack_coeff;
    SmoothedValue m_smoothed_release_coeff;
};

template <>
inline float LimiterImpl::get_parameter<float>(Parameter p, uint32_t modulation_offset) {
    return get_parameter_float(p, modulation_offset);
}

}  // namespace thl::inline THL_ABI_NAMESPACE::dsp::utils
