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

    void Init() {
        std::fill(&line_[0], &line_[max_delay], T(0));
        delay_ = 1;
        write_ptr_ = 0;
    }

    void set_delay(size_t delay) {
        delay_ = delay;
    }

    void Write(T sample) {
        line_[write_ptr_] = sample;
        write_ptr_ = (write_ptr_ - 1 + max_delay) % max_delay;
    }

    T Allpass(T sample, size_t delay, T coefficient) {
        const float read = line_[(write_ptr_ + delay) % max_delay];
        const float write = sample + coefficient * read;
        Write(static_cast<T>(write));
        return static_cast<T>(-write * coefficient + read);
    }

    T WriteRead(T sample, float delay) {
        Write(sample);
        return Read(delay);
    }

    T Read() const {
        return line_[(write_ptr_ + delay_) % max_delay];
    }

    T Read(size_t delay) const {
        return line_[(write_ptr_ + delay) % max_delay];
    }

    T Read(float delay) const {
        const auto [delay_integral, delay_fractional] = detail::split_integral_fractional(delay);
        const T a = line_[(write_ptr_ + static_cast<size_t>(delay_integral)) % max_delay];
        const T b = line_[(write_ptr_ + static_cast<size_t>(delay_integral + 1)) % max_delay];
        return a + (b - a) * delay_fractional;
    }

    T ReadHermite(float delay) const {
        const auto [delay_integral, delay_fractional] = detail::split_integral_fractional(delay);
        const int32_t t = static_cast<int32_t>(write_ptr_) + delay_integral + static_cast<int32_t>(max_delay);
        const T xm1 = line_[static_cast<size_t>(t - 1) % max_delay];
        const T x0 = line_[static_cast<size_t>(t) % max_delay];
        const T x1 = line_[static_cast<size_t>(t + 1) % max_delay];
        const T x2 = line_[static_cast<size_t>(t + 2) % max_delay];
        const float c = (x1 - xm1) * 0.5f;
        const float v = x0 - x1;
        const float w = c + v;
        const float a = w + v + (x2 - x0) * 0.5f;
        const float b_neg = w + a;
        const float f = delay_fractional;
        return static_cast<T>((((a * f) - b_neg) * f + c) * f + x0);
    }

private:
    size_t write_ptr_ = 0;
    size_t delay_ = 1;
    T line_[max_delay] {};

    DelayLine(const DelayLine&) = delete;
    DelayLine& operator=(const DelayLine&) = delete;
};

} // namespace thl::dsp::utils
