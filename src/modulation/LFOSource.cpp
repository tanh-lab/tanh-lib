#include "tanh/modulation/LFOSource.h"

#include <cmath>
#include <tanh/core/Numbers.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

using namespace thl::modulation;

void LFOSourceImpl::prepare(double sample_rate, size_t samples_per_block) {
    m_sample_rate = sample_rate;
    m_phase_increment = static_cast<float>(get_parameter<float>(Frequency) / sample_rate);
    resize_buffers(samples_per_block);
    m_samples_until_update = 0;
}

void LFOSourceImpl::process(size_t num_samples) {
    m_change_points.clear();

    for (size_t i = 0; i < num_samples; ++i) {
        float freq = get_parameter<float>(Frequency, static_cast<uint32_t>(i));

        m_phase_increment = static_cast<float>(freq / m_sample_rate);

        if (m_samples_until_update == 0) {
            auto decimation =
                static_cast<uint32_t>(get_parameter<int>(Decimation, static_cast<uint32_t>(i)));
            auto waveform =
                static_cast<LFOWaveform>(get_parameter<int>(Waveform, static_cast<uint32_t>(i)));
            m_last_output = generate_sample(m_phase, waveform);
            m_samples_until_update = decimation;
            m_change_points.push_back(static_cast<uint32_t>(i));
        }
        --m_samples_until_update;

        m_output_buffer[i] = m_last_output;

        m_phase += m_phase_increment;
        if (m_phase >= 1.0f) { m_phase -= 1.0f; }
    }
}

void LFOSourceImpl::process_single(float* out, uint32_t sample_index) {
    float freq = get_parameter<float>(Frequency, sample_index);
    auto waveform = static_cast<LFOWaveform>(get_parameter<int>(Waveform, sample_index));
    auto decimation = static_cast<uint32_t>(get_parameter<int>(Decimation, sample_index));

    m_phase_increment = static_cast<float>(freq / m_sample_rate);

    if (m_samples_until_update == 0) {
        m_last_output = generate_sample(m_phase, waveform);
        m_samples_until_update = decimation;
        record_change_point(sample_index);
    }
    --m_samples_until_update;

    *out = m_last_output;
    m_output_buffer[sample_index] = m_last_output;

    m_phase += m_phase_increment;
    if (m_phase >= 1.0f) { m_phase -= 1.0f; }
}

float LFOSourceImpl::generate_sample(float phase, LFOWaveform waveform) {
    constexpr float k_two_pi = 2.0f * std::numbers::pi_v<float>;

    switch (waveform) {
        case LFOWaveform::Sine: return std::sin(phase * k_two_pi);

        case LFOWaveform::Triangle:
            if (phase < 0.25f) { return phase * 4.0f; }
            if (phase < 0.75f) { return 2.0f - phase * 4.0f; }
            return phase * 4.0f - 4.0f;

        case LFOWaveform::Saw: return 2.0f * phase - 1.0f;

        case LFOWaveform::Square: return phase < 0.5f ? 1.0f : -1.0f;

        default: return 0.0f;
    }
}
