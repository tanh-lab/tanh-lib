#include <tanh/core/AbiNamespace.h>
#include <tanh/core/BufferView.h>
#include <tanh/dsp/utils/Decibels.h>
#include <tanh/dsp/utils/Limiter.h>
#include <tanh/dsp/utils/ParameterValue.h>
#include <tanh/dsp/utils/TimeConstant.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace thl::inline THL_ABI_NAMESPACE::dsp::utils {

namespace {
// 5 ms linear ramp on threshold — long enough to mask sub-block step changes
// from control-rate modulation, short enough that intentional automation
// still tracks closely.
constexpr double k_threshold_smoothing_time = 0.005;
constexpr float k_max_threshold_db = 24.0f;
constexpr float k_min_time_ms = 0.01f;
constexpr float k_max_time_ms = 60000.0f;
}  // namespace

LimiterImpl::LimiterImpl() = default;
LimiterImpl::~LimiterImpl() = default;

void LimiterImpl::read_parameters(uint32_t modulation_offset) TANH_NONBLOCKING_FUNCTION {
    // NaN keeps the last value: one bad automation point must not make the gain NaN for good.
    m_threshold_db = sanitised(get_parameter<float>(Threshold, modulation_offset),
                               m_threshold_db,
                               k_min_db,
                               k_max_threshold_db);
    m_attack_ms = sanitised(get_parameter<float>(Attack, modulation_offset),
                            m_attack_ms,
                            k_min_time_ms,
                            k_max_time_ms);
    m_release_ms = sanitised(get_parameter<float>(Release, modulation_offset),
                             m_release_ms,
                             k_min_time_ms,
                             k_max_time_ms);
    m_smoothed_threshold.set_target_value(db_to_gain(m_threshold_db));
    m_smoothed_attack_coeff.set_target_value(one_pole_pole(m_attack_ms, m_sample_rate));
    m_smoothed_release_coeff.set_target_value(one_pole_pole(m_release_ms, m_sample_rate));
}

void LimiterImpl::prepare(const double& sample_rate,
                          const size_t& /*samples_per_block*/,
                          const size_t& /*num_channels*/) {
    m_sample_rate = sample_rate;
    m_gain = 1.0f;

    m_smoothed_threshold.reset(sample_rate, k_threshold_smoothing_time);
    m_smoothed_attack_coeff.reset(sample_rate, k_threshold_smoothing_time);
    m_smoothed_release_coeff.reset(sample_rate, k_threshold_smoothing_time);

    // Prime with the current parameter values so the first block doesn't ramp from zero.
    read_parameters(0);
    m_smoothed_threshold.set_current_and_target_value(db_to_gain(m_threshold_db));
    m_smoothed_attack_coeff.set_current_and_target_value(one_pole_pole(m_attack_ms, sample_rate));
    m_smoothed_release_coeff.set_current_and_target_value(one_pole_pole(m_release_ms, sample_rate));
}

void LimiterImpl::process(thl::core::BufferView buffer,
                          uint32_t modulation_offset) TANH_NONBLOCKING_FUNCTION {
    constexpr size_t k_max_channels = 16;
    const size_t num_samples = buffer.get_num_samples();
    const size_t num_channels = std::min(buffer.get_num_channels(), k_max_channels);
    std::array<float*, k_max_channels> channel_ptrs;
    for (size_t ch = 0; ch < num_channels; ++ch) {
        channel_ptrs[ch] = buffer.get_write_pointer(ch);
    }

    read_parameters(modulation_offset);

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

}  // namespace thl::inline THL_ABI_NAMESPACE::dsp::utils
