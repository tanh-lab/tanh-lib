#include <tanh/dsp/utils/DynamicDelayLine.h>

#include <cstdint>

#include <tanh/dsp/utils/DspMath.h>

namespace thl::dsp::utils {

DynamicDelayLine::DynamicDelayLine() = default;
DynamicDelayLine::~DynamicDelayLine() = default;

void DynamicDelayLine::prepare(size_t max_delay) {
    m_max_delay = max_delay;
    m_buf.resize(1, max_delay);
    m_buf.clear();
    m_write_ptr = 0;
    m_delay = 1;
}

void DynamicDelayLine::reset() {
    m_buf.clear();
    m_write_ptr = 0;
}

void DynamicDelayLine::set_delay(size_t delay) {
    m_delay = delay;
}

void DynamicDelayLine::write(float sample) {
    m_buf.get_write_pointer(0)[m_write_ptr] = sample;
    m_write_ptr = (m_write_ptr + m_max_delay - 1) % m_max_delay;
}

float DynamicDelayLine::write_read(float sample, float delay) {
    write(sample);
    return read(delay);
}

float DynamicDelayLine::read() const {
    return read_at(m_delay);
}

float DynamicDelayLine::read(size_t delay) const {
    return read_at(delay);
}

float DynamicDelayLine::read(float delay) const {
    const auto [i, f] = split_integral_fractional(delay);
    const float a = read_at(static_cast<size_t>(i));
    const float b = read_at(static_cast<size_t>(i) + 1);
    return a + (b - a) * f;
}

float DynamicDelayLine::read_hermite(float delay) const {
    const auto [i, f] = split_integral_fractional(delay);
    const int32_t t = static_cast<int32_t>(m_write_ptr) + i + static_cast<int32_t>(m_max_delay);
    const float xm1 = m_buf.get_read_pointer(0)[static_cast<size_t>(t - 1) % m_max_delay];
    const float x0 = m_buf.get_read_pointer(0)[static_cast<size_t>(t) % m_max_delay];
    const float x1 = m_buf.get_read_pointer(0)[static_cast<size_t>(t + 1) % m_max_delay];
    const float x2 = m_buf.get_read_pointer(0)[static_cast<size_t>(t + 2) % m_max_delay];
    const float c = (x1 - xm1) * 0.5f;
    const float v = x0 - x1;
    const float w = c + v;
    const float a_val = w + v + (x2 - x0) * 0.5f;
    const float b_neg = w + a_val;
    return (((a_val * f) - b_neg) * f + c) * f + x0;
}

float DynamicDelayLine::tap(float offset) const {
    return read(offset + 1.0f);
}

float DynamicDelayLine::tap(size_t offset) const {
    return read_at(offset + 1);
}

float DynamicDelayLine::read_at(size_t delay) const {
    return m_buf.get_read_pointer(0)[(m_write_ptr + delay) % m_max_delay];
}

}  // namespace thl::dsp::utils
