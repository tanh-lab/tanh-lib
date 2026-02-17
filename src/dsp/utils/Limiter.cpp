#include <tanh/dsp/utils/Limiter.h>
#include <cmath>
#include <algorithm>

namespace thl::dsp::utils {

LimiterImpl::LimiterImpl() = default;
LimiterImpl::~LimiterImpl() = default;

void LimiterImpl::prepare(const double& sample_rate, const size_t& /*samples_per_block*/, const size_t& num_channels) {
    m_sample_rate = sample_rate;
    m_channels = num_channels;
    m_gain = 1.0f;
}

void LimiterImpl::process(float** buffer, const size_t& num_samples, const size_t& num_channels) {
    float threshold_db = get_parameter<float>(Threshold);
    float threshold = std::pow(10.0f, threshold_db / 20.0f);
    float attack_ms = std::max(get_parameter<float>(Attack), 0.01f);
    float release_ms = std::max(get_parameter<float>(Release), 0.01f);

    float attack_coeff = std::exp(-1.0 / (attack_ms * 0.001 * m_sample_rate));
    float release_coeff = std::exp(-1.0 / (release_ms * 0.001 * m_sample_rate));

    for (size_t i = 0; i < num_samples; ++i) {
        float peak = 0.0f;
        for (size_t ch = 0; ch < num_channels; ++ch) {
            peak = std::max(peak, std::fabs(buffer[ch][i]));
        }

        float target_gain = (peak > threshold) ? threshold / peak : 1.0f;

        if (target_gain < m_gain) {
            m_gain = attack_coeff * m_gain + (1.0f - attack_coeff) * target_gain;
        } else {
            m_gain = release_coeff * m_gain + (1.0f - release_coeff) * target_gain;
        }

        for (size_t ch = 0; ch < num_channels; ++ch) {
            buffer[ch][i] *= m_gain;
        }
    }
}

} // namespace thl::dsp::utils
