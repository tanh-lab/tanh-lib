#include "tanh/dsp/synth/SineProcessor.h"

using namespace thl::dsp::synth;

SineProcessor::SineProcessor()
{
    // Initialize the parameters from the selected code
    m_phase.resize(2);
    m_phase[0] = 0.f;
    m_phase[1] = 0.f;
}

SineProcessor::~SineProcessor() = default;

void SineProcessor::set_parameter(const Parameters& param, float value)
{
    switch (param) {
        case FREQUENCY:
            m_frequency = value;
            break;
        case AMPLITUDE:
            m_amplitude = value;
            break;
        default:
            break;
    }
}

float SineProcessor::get_parameter(const Parameters& param) const
{
    switch (param) {
        case FREQUENCY:
            return m_frequency;
        case AMPLITUDE:
            return m_amplitude;
        default:
            return 0.f;
    }
}

void SineProcessor::prepare(const float& sample_rate, const int& samples_per_block)
{
    m_sample_rate = sample_rate;
    m_samples_per_block = samples_per_block;
}

void SineProcessor::process(float* output_buffer, unsigned int n_buffer_frames)
{
    float* out = output_buffer;
    
    float phase_increment = 2.0f * (float) M_PI * m_frequency / (float) m_sample_rate;

    for (unsigned int i = 0; i < n_buffer_frames; i++) {
        float sample_left = m_amplitude * std::sin(m_phase[0]);
        float sample_right = m_amplitude * std::sin(m_phase[1]);
        out[i] = sample_left;  // left channel
        out[i + n_buffer_frames] = sample_right;  // right channel

        m_phase[0] += phase_increment;
        m_phase[1] += phase_increment * 1.5f;
        if (m_phase[0] >= 2.0f * (float) M_PI) {
            m_phase[0] -= 2.0f * (float) M_PI;
        }
        if (m_phase[1] >= 2.0f * (float) M_PI) {
            m_phase[1] -= 2.0f * (float) M_PI;
        }
    }
}
