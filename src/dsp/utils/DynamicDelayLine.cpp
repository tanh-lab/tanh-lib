#include <tanh/dsp/utils/DspMath.h>
#include <tanh/dsp/utils/DynamicDelayLine.h>

#include <algorithm>
#include <bit>
#include <cstddef>

namespace thl::dsp::utils {

DynamicDelayLine::DynamicDelayLine() = default;
DynamicDelayLine::~DynamicDelayLine() = default;

void DynamicDelayLine::prepare(size_t max_delay) {
    m_max_delay = max_delay;
    // Power-of-two capacity so wrapping is a mask instead of a division.
    m_capacity = std::bit_ceil(std::max<size_t>(max_delay, 1));
    m_mask = m_capacity - 1;
    m_buf.resize(1, m_capacity);
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
    // Unsigned underflow at 0 wraps to all-ones; the mask folds it to capacity-1.
    m_write_ptr = (m_write_ptr - 1) & m_mask;
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
    const size_t t = m_write_ptr + static_cast<size_t>(i);
    const float* buf = m_buf.get_read_pointer(0);
    const float xm1 = buf[(t - 1) & m_mask];
    const float x0 = buf[t & m_mask];
    const float x1 = buf[(t + 1) & m_mask];
    const float x2 = buf[(t + 2) & m_mask];
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
    return m_buf.get_read_pointer(0)[(m_write_ptr + delay) & m_mask];
}

}  // namespace thl::dsp::utils
