#include "tanh/dsp/synth/SineProcessor.h"

#include <algorithm>

using namespace thl::dsp::synth;

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

SineProcessorImpl::SineProcessorImpl() = default;
SineProcessorImpl::~SineProcessorImpl() = default;

void SineProcessorImpl::prepare(const double& sample_rate,
                                const size_t& samples_per_block,
                                const size_t& num_channels) {
    m_phase.resize(num_channels, 0.f);

    m_sample_rate = sample_rate;
    m_samples_per_block = samples_per_block;

    m_smoothed_frequency.reset(sample_rate, 0.005, utils::SmoothedValue::Linear);
    m_smoothed_amplitude.reset(sample_rate, 0.005, utils::SmoothedValue::Linear);

    m_smoothed_frequency.set_current_and_target_value(get_parameter<float>(Frequency));
    m_smoothed_amplitude.set_current_and_target_value(get_parameter<float>(Amplitude));
}

void SineProcessorImpl::process(thl::dsp::audio::AudioBufferView buffer,
                                uint32_t modulation_offset) {
    constexpr size_t k_max_channels = 16;
    const size_t num_samples = buffer.get_num_frames();
    const size_t num_channels = std::min(buffer.get_num_channels(), k_max_channels);
    float* channel_ptrs[k_max_channels];
    for (size_t ch = 0; ch < num_channels; ++ch) {
        channel_ptrs[ch] = buffer.get_write_pointer(ch);
    }

    constexpr float k_two_pi = 2.0f * static_cast<float>(M_PI);
    const auto sample_rate_f = static_cast<float>(m_sample_rate);

    m_smoothed_frequency.set_target_value(get_parameter<float>(Frequency, modulation_offset));
    m_smoothed_amplitude.set_target_value(get_parameter<float>(Amplitude, modulation_offset));

    for (size_t i = 0; i < num_samples; ++i) {
        const float current_freq = m_smoothed_frequency.get_smoothed_value(1);
        const float current_amp = m_smoothed_amplitude.get_smoothed_value(1);

        const float phase_increment = k_two_pi * current_freq / sample_rate_f;

        for (size_t ch = 0; ch < num_channels; ++ch) {
            float& phase = m_phase[ch];

            channel_ptrs[ch][i] = current_amp * std::sin(phase);
            phase += phase_increment;

            if (phase >= k_two_pi) {
                phase -= k_two_pi;
            } else if (phase < 0.0f) {
                phase += k_two_pi;
            }
        }
    }
}
