#include "tanh/dsp/synth/SineProcessor.h"

using namespace thl::dsp::synth;

#ifndef M_PI
    #define M_PI 3.14159265358979323846
#endif

SineProcessor::SineProcessor() = default;
SineProcessor::~SineProcessor() = default;

void SineProcessor::prepare(const double& sample_rate, const size_t& samples_per_block, const size_t& num_channels) {
    m_phase.resize(num_channels, 0.f);

    m_sample_rate = sample_rate;
    m_samples_per_block = samples_per_block;

    smoothed_frequency.reset(sample_rate, 0.005, utils::SmoothedValue::Linear);
    smoothed_amplitude.reset(sample_rate, 0.005, utils::SmoothedValue::Linear);

    smoothed_frequency.set_current_and_target_value(m_frequency);
    smoothed_amplitude.set_current_and_target_value(m_amplitude);
}

void SineProcessor::process(float** buffer, const size_t& num_samples, const size_t& num_channels) {
        constexpr float two_pi = 2.0f * static_cast<float>(M_PI);
        const auto sample_rate_f = static_cast<float>(m_sample_rate);

        for (size_t i = 0; i < num_samples; ++i) {
            const float current_freq = smoothed_frequency.get_smoothed_value(1);
            const float current_amp  = smoothed_amplitude.get_smoothed_value(1);

            const float phase_increment = two_pi * current_freq / sample_rate_f;

            for (size_t ch = 0; ch < num_channels; ++ch) {
                float& phase = m_phase[ch];

                buffer[ch][i] = current_amp * std::sin(phase);
                phase += phase_increment;

                if (phase >= two_pi)
                    phase -= two_pi;
                else if (phase < 0.0f)
                    phase += two_pi;
            }
        }
    }

void SineProcessor::set_parameter(Parameters param, float value) {
    set_parameter(static_cast<int>(param), value);
}

float SineProcessor::get_parameter(Parameters param) const {
    return get_parameter(static_cast<int>(param));
}

void SineProcessor::set_parameter(int param_id, float value) {
    switch (param_id) {
        case FREQUENCY:
            m_frequency = value;
            smoothed_frequency.set_target_value(m_frequency);
            break;
        case AMPLITUDE:
            m_amplitude = value;
            smoothed_amplitude.set_target_value(m_amplitude);
            break;
        default:
            break;
    }
}

float SineProcessor::get_parameter(int param_id) const {
    switch (param_id) {
        case FREQUENCY:
            return m_frequency;
        case AMPLITUDE:
            return m_amplitude;
        default:
            return 0.f;
    }
}
