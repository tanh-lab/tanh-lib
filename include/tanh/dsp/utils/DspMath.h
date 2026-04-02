#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace thl::dsp::utils {

inline float interpolate(const float* table, float index, float size) {
    index *= size;
    const int32_t index_integral = static_cast<int32_t>(index);
    const float index_fractional = index - static_cast<float>(index_integral);
    const float a = table[index_integral];
    const float b = table[index_integral + 1];
    return a + (b - a) * index_fractional;
}

inline float interpolate_wrap(const float* table, float index, float size) {
    index -= static_cast<float>(static_cast<int32_t>(index));
    index *= size;
    const int32_t index_integral = static_cast<int32_t>(index);
    const float index_fractional = index - static_cast<float>(index_integral);
    const float a = table[index_integral];
    const float b = table[index_integral + 1];
    return a + (b - a) * index_fractional;
}

inline float crossfade(float a, float b, float fade) {
    return a + (b - a) * fade;
}

inline float soft_limit(float x) {
    return x * (27.0f + x * x) / (27.0f + 9.0f * x * x);
}

inline float soft_clip(float x) {
    if (x < -3.0f) { return -1.0f; }
    if (x > 3.0f) { return 1.0f; }
    return soft_limit(x);
}

inline int32_t clip16(int32_t x) {
    return std::clamp<int32_t>(x, -32768, 32767);
}

inline uint16_t clip_u16(int32_t x) {
    return static_cast<uint16_t>(std::clamp<int32_t>(x, 0, 65535));
}

inline float sqrt(float x) {
    return std::sqrt(x);
}

inline int16_t soft_convert(float x) {
    return static_cast<int16_t>(clip16(static_cast<int32_t>(soft_limit(x * 0.5f) * 32768.0f)));
}

inline float semitones_to_ratio(float semitones) {
    return std::exp2(semitones / 12.0f);
}

template <typename T>
inline void one_pole(T& out, const T& in, const T& coefficient) {
    out += coefficient * (in - out);
}

template <typename T>
inline void slope(T& out, const T& in, const T& positive, const T& negative) {
    T error = in - out;
    out += (error > static_cast<T>(0) ? positive : negative) * error;
}

template <typename T>
inline void slew(T& out, const T& in, const T& delta) {
    T error = in - out;
    if (error > delta) {
        error = delta;
    } else if (error < -delta) {
        error = -delta;
    }
    out += error;
}

template <typename T>
inline void constrain(T& value, const T& min, const T& max) {
    if (value < min) {
        value = min;
    } else if (value > max) {
        value = max;
    }
}

struct IntegralFractional {
    int32_t integral;
    float fractional;
};

inline IntegralFractional split_integral_fractional(float x) {
    int32_t i = static_cast<int32_t>(x);
    return {i, x - static_cast<float>(i)};
}

}  // namespace thl::dsp::utils
