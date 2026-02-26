#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace thl::dsp::utils {

namespace detail {
struct IntegralFractional {
    int32_t integral;
    float fractional;
};

inline IntegralFractional split_integral_fractional(float x) {
    const int32_t integral = static_cast<int32_t>(x);
    return {integral, x - static_cast<float>(integral)};
}
} // namespace detail

template <typename T, size_t max_delay>
class DelayLine {
public:
    DelayLine() = default;

    void init() {
        std::fill(&m_line[0], &m_line[max_delay], T(0));
        m_delay = 1;
        m_write_ptr = 0;
    }

    void set_delay(size_t delay) {
        m_delay = delay;
    }

    void write(T sample) {
        m_line[m_write_ptr] = sample;
        m_write_ptr = (m_write_ptr - 1 + max_delay) % max_delay;
    }

    T allpass(T sample, size_t delay, T coefficient) {
        const float read = m_line[(m_write_ptr + delay) % max_delay];
        const float w = sample + coefficient * read;
        write(static_cast<T>(w));
        return static_cast<T>(-w * coefficient + read);
    }

    T write_read(T sample, float delay) {
        write(sample);
        return read(delay);
    }

    T read() const {
        return m_line[(m_write_ptr + m_delay) % max_delay];
    }

    T read(size_t delay) const {
        return m_line[(m_write_ptr + delay) % max_delay];
    }

    T read(float delay) const {
        const auto [delay_integral, delay_fractional] = detail::split_integral_fractional(delay);
        const T a = m_line[(m_write_ptr + static_cast<size_t>(delay_integral)) % max_delay];
        const T b = m_line[(m_write_ptr + static_cast<size_t>(delay_integral + 1)) % max_delay];
        return a + (b - a) * delay_fractional;
    }

    T read_hermite(float delay) const {
        const auto [delay_integral, delay_fractional] = detail::split_integral_fractional(delay);
        const int32_t t = static_cast<int32_t>(m_write_ptr) + delay_integral + static_cast<int32_t>(max_delay);
        const T xm1 = m_line[static_cast<size_t>(t - 1) % max_delay];
        const T x0 = m_line[static_cast<size_t>(t) % max_delay];
        const T x1 = m_line[static_cast<size_t>(t + 1) % max_delay];
        const T x2 = m_line[static_cast<size_t>(t + 2) % max_delay];
        const float c = (x1 - xm1) * 0.5f;
        const float v = x0 - x1;
        const float w = c + v;
        const float a = w + v + (x2 - x0) * 0.5f;
        const float b_neg = w + a;
        const float f = delay_fractional;
        return static_cast<T>((((a * f) - b_neg) * f + c) * f + x0);
    }

private:
    size_t m_write_ptr = 0;
    size_t m_delay = 1;
    T m_line[max_delay] {};

    DelayLine(const DelayLine&) = delete;
    DelayLine& operator=(const DelayLine&) = delete;
};

} // namespace thl::dsp::utils
