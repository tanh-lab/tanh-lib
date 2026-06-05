#include <tanh/dsp/utils/Limiter.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "tanh/dsp/audio/AudioBufferView.h"

namespace thl::dsp::utils {

namespace {
// 5 ms linear ramp on threshold — long enough to mask sub-block step changes
// from control-rate modulation, short enough that intentional automation
// still tracks closely.
constexpr double k_threshold_smoothing_time = 0.005;
}  // namespace

LimiterImpl::LimiterImpl() = default;
LimiterImpl::~LimiterImpl() = default;

void LimiterImpl::prepare(const double& sample_rate,
                          const size_t& /*samples_per_block*/,
                          const size_t& num_channels) {
    m_sample_rate = sample_rate;
    m_channels = num_channels;
    m_gain = 1.0f;

    m_smoothed_threshold.reset(sample_rate, k_threshold_smoothing_time);
    m_smoothed_attack_coeff.reset(sample_rate, k_threshold_smoothing_time);
    m_smoothed_release_coeff.reset(sample_rate, k_threshold_smoothing_time);

    // Prime with the current parameter values so the first block doesn't
    // ramp from zero.
    const float threshold_db = get_parameter<float>(Threshold);
    const float attack_ms = std::max(get_parameter<float>(Attack), 0.01f);
    const float release_ms = std::max(get_parameter<float>(Release), 0.01f);
    m_smoothed_threshold.set_current_and_target_value(std::pow(10.0f, threshold_db / 20.0f));
    m_smoothed_attack_coeff.set_current_and_target_value(
        static_cast<float>(std::exp(-1.0 / (attack_ms * 0.001 * sample_rate))));
    m_smoothed_release_coeff.set_current_and_target_value(
        static_cast<float>(std::exp(-1.0 / (release_ms * 0.001 * sample_rate))));
}

void LimiterImpl::process(thl::dsp::audio::AudioBufferView buffer, uint32_t modulation_offset) {
    constexpr size_t k_max_channels = 16;
    const size_t num_samples = buffer.get_num_frames();
    const size_t num_channels = std::min(buffer.get_num_channels(), k_max_channels);
    std::array<float*, k_max_channels> channel_ptrs;
    for (size_t ch = 0; ch < num_channels; ++ch) {
        channel_ptrs[ch] = buffer.get_write_pointer(ch);
    }

    const float threshold_db = get_parameter<float>(Threshold, modulation_offset);
    m_smoothed_threshold.set_target_value(std::pow(10.0f, threshold_db / 20.0f));

    float const attack_ms = std::max(get_parameter<float>(Attack, modulation_offset), 0.01f);
    float const release_ms = std::max(get_parameter<float>(Release, modulation_offset), 0.01f);

    m_smoothed_attack_coeff.set_target_value(
        static_cast<float>(std::exp(-1.0 / (attack_ms * 0.001 * m_sample_rate))));
    m_smoothed_release_coeff.set_target_value(
        static_cast<float>(std::exp(-1.0 / (release_ms * 0.001 * m_sample_rate))));

    for (size_t i = 0; i < num_samples; ++i) {
        const float threshold = m_smoothed_threshold.get_smoothed_value(1);
        const float attack_coeff = m_smoothed_attack_coeff.get_smoothed_value(1);
        const float release_coeff = m_smoothed_release_coeff.get_smoothed_value(1);

        float peak = 0.0f;
        for (size_t ch = 0; ch < num_channels; ++ch) {
            peak = std::max(peak, std::fabs(channel_ptrs[ch][i]));
        }

        float const target_gain = (peak > threshold) ? threshold / peak : 1.0f;

        if (target_gain < m_gain) {
            m_gain = attack_coeff * m_gain + (1.0f - attack_coeff) * target_gain;
        } else {
            m_gain = release_coeff * m_gain + (1.0f - release_coeff) * target_gain;
        }

        for (size_t ch = 0; ch < num_channels; ++ch) { channel_ptrs[ch][i] *= m_gain; }
    }
}

}  // namespace thl::dsp::utils
