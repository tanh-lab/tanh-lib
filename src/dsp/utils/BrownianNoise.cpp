#include <tanh/dsp/utils/BrownianNoise.h>

#include <algorithm>
#include <cmath>
#include <tanh/core/Numbers.h>

#include <tanh/dsp/utils/Random.h>

namespace thl::dsp::utils {

BrownianNoise::BrownianNoise() = default;

void BrownianNoise::prepare(float rate, float sample_rate, float momentum) {
    m_rate = rate;
    m_sample_rate = sample_rate;
    m_momentum = momentum;
    m_smooth_coeff = calc_coeff(rate, sample_rate);
    reset();
}

void BrownianNoise::set_rate(float rate) {
    m_rate = rate;
    m_smooth_coeff = calc_coeff(rate, m_sample_rate);
}

void BrownianNoise::set_momentum(float momentum) {
    m_momentum = std::clamp(momentum, 0.0f, 1.0f);
}

void BrownianNoise::reset() {
    m_value = 0.0f;
    m_velocity = 0.0f;
    m_smoothed = 0.0f;
    m_phase = 0.0f;
}

float BrownianNoise::process() {
    m_phase += m_rate / m_sample_rate;
    if (m_phase >= 1.0f) {
        m_phase -= std::floor(m_phase);
        m_velocity += (Random::get_float() * 2.0f - 1.0f) * 0.3f;
        m_velocity -= m_value * m_momentum;
        m_velocity *= (1.0f - m_momentum * 0.5f);
    }
    m_value += m_velocity * (m_rate / m_sample_rate);
    m_value = std::tanh(m_value);
    m_smoothed += m_smooth_coeff * (m_value - m_smoothed);
    return m_smoothed;
}

float BrownianNoise::calc_coeff(float rate, float sample_rate) {
    return 1.0f - std::exp(-2.0f * std::numbers::pi_v<float> * (rate * 4.0f) / sample_rate);
}

}  // namespace thl::dsp::utils
