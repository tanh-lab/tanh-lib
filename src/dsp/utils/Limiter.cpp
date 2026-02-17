#include <tanh/dsp/utils/Limiter.h>
#include <cmath>

namespace thl::dsp::utils {

LimiterImpl::LimiterImpl() = default;
LimiterImpl::~LimiterImpl() = default;

void LimiterImpl::prepare(const double& sample_rate, const size_t& /*samples_per_block*/, const size_t& num_channels) {
    m_sample_rate = sample_rate;
    m_channels = num_channels;
}

float LimiterImpl::tanh_limit(float input, float threshold) {
    float abs_in = std::fabs(input);
    if (abs_in <= threshold) return input;
    float sign = (input > 0.0f) ? 1.0f : -1.0f;
    float excess = (abs_in - threshold) / (1.0f - threshold);
    return sign * (threshold + (1.0f - threshold) * std::tanh(excess));
}

void LimiterImpl::process(float** buffer, const size_t& num_samples, const size_t& num_channels) {
    float threshold = get_parameter<float>(ThresholdGain);

    for (size_t ch = 0; ch < num_channels; ++ch) {
        float* channel = buffer[ch];
        for (size_t i = 0; i < num_samples; ++i) {
            channel[i] = tanh_limit(channel[i], threshold);
        }
    }
}

} // namespace thl::dsp::utils
