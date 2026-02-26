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
    if (x < -3.0f) return -1.0f;
    if (x > 3.0f) return 1.0f;
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

float semitones_to_ratio(float semitones);

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

} // namespace thl::dsp::utils

// Temporary compatibility macros to keep the Rings vendoring changes mechanical.
#ifndef MAKE_INTEGRAL_FRACTIONAL
#define MAKE_INTEGRAL_FRACTIONAL(x)                                      \
    int32_t x ## _integral = static_cast<int32_t>(x);                    \
    float x ## _fractional = (x) - static_cast<float>(x ## _integral);
#endif

#ifndef ONE_POLE
#define ONE_POLE(out, in, coefficient) (out) += (coefficient) * ((in) - (out))
#endif

#ifndef SLOPE
#define SLOPE(out, in, positive, negative) {                             \
    float error = (in) - (out);                                          \
    (out) += (error > 0.0f ? (positive) : (negative)) * error;           \
}
#endif

#ifndef SLEW
#define SLEW(out, in, delta) {                                           \
    float error = (in) - (out);                                          \
    float d = (delta);                                                   \
    if (error > d) {                                                     \
        error = d;                                                       \
    } else if (error < -d) {                                             \
        error = -d;                                                      \
    }                                                                    \
    (out) += error;                                                      \
}
#endif

#ifndef CONSTRAIN
#define CONSTRAIN(var, min, max) {                                        \
    if ((var) < (min)) {                                                  \
        (var) = (min);                                                    \
    } else if ((var) > (max)) {                                           \
        (var) = (max);                                                    \
    }                                                                     \
}
#endif
