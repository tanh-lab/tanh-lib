#pragma once

#include <tanh/core/AbiNamespace.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <cmath>
#include <concepts>
#include <numbers>

namespace thl::inline THL_ABI_NAMESPACE::dsp::utils {

/// The floor of gain_to_db() and power_to_db() by default: -120 dB.
inline constexpr float k_min_db = -120.0f;

/// The gain (amplitude ratio) of @p db decibels: 10^(db / 20).
template <std::floating_point T>
[[nodiscard]] inline T db_to_gain(T db) TANH_NONBLOCKING_FUNCTION {
    return std::exp(db * (std::numbers::ln10_v<T> / static_cast<T>(20)));
}

/// The level in dB of the gain (amplitude ratio, >= 0) @p gain, at least @p floor_db.
template <std::floating_point T>
[[nodiscard]] inline T gain_to_db(T gain,
                                  T floor_db = static_cast<T>(k_min_db)) TANH_NONBLOCKING_FUNCTION {
    return gain > static_cast<T>(0) ? std::max(static_cast<T>(20) * std::log10(gain), floor_db)
                                    : floor_db;
}

/// The level in dB of the power (a squared amplitude, >= 0) @p power, at least @p floor_db.
template <std::floating_point T>
[[nodiscard]] inline T power_to_db(T power, T floor_db = static_cast<T>(k_min_db))
    TANH_NONBLOCKING_FUNCTION {
    return power > static_cast<T>(0) ? std::max(static_cast<T>(10) * std::log10(power), floor_db)
                                     : floor_db;
}

}  // namespace thl::inline THL_ABI_NAMESPACE::dsp::utils
