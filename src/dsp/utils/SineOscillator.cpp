#include <tanh/dsp/utils/SineOscillator.h>

#include <cmath>
#include <tanh/core/Numbers.h>

namespace thl::dsp::utils {

SineOscillator::SineOscillator() = default;

void SineOscillator::prepare(float frequency, float sample_rate, float amplitude) {
    m_amplitude = amplitude;
    m_w = 2.0f * std::sin(std::numbers::pi_v<float> * frequency / sample_rate);
    m_s1 = 0.0f;
    m_s2 = amplitude;  // phase=0: sin(0)=0, cos(0)=amplitude
}

void SineOscillator::set_frequency(float frequency, float sample_rate) {
    m_w = 2.0f * std::sin(std::numbers::pi_v<float> * frequency / sample_rate);
}

void SineOscillator::reset() {
    m_s1 = 0.0f;
    m_s2 = m_amplitude;
}

float SineOscillator::process() {
    m_s1 = m_s1 + m_w * m_s2;
    m_s2 = m_s2 - m_w * m_s1;
    return m_s1;
}

}  // namespace thl::dsp::utils
