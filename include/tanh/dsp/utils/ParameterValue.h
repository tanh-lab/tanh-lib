#pragma once

#include <tanh/core/AbiNamespace.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <cmath>
#include <type_traits>

namespace thl::inline THL_ABI_NAMESPACE::dsp::utils {

/// A parameter's default and range.
struct ParameterRange {
    float m_default = 0.0f;
    float m_min = 0.0f;
    float m_max = 1.0f;
};

/// @p value clamped to [@p lowest, @p highest]; NaN becomes @p fallback. Infinities clamp, so a
/// range up to infinity (a ratio) keeps them.
[[nodiscard]] inline float sanitised(float value, float fallback, float lowest, float highest)
    TANH_NONBLOCKING_FUNCTION {
    return std::isnan(value) ? fallback : std::clamp(value, lowest, highest);
}

/// sanitised() with @p range's default and bounds.
[[nodiscard]] inline float sanitised(float value,
                                     const ParameterRange& range) TANH_NONBLOCKING_FUNCTION {
    return sanitised(value, range.m_default, range.m_min, range.m_max);
}

/// An enum value from a parameter's float: rounded, clamped to [0, @p last]; NaN becomes
/// @p fallback. For enums numbered from 0 without gaps.
template <typename Enum>
    requires std::is_enum_v<Enum>
[[nodiscard]] inline Enum to_enum(float value, Enum last, Enum fallback) TANH_NONBLOCKING_FUNCTION {
    if (std::isnan(value)) { return fallback; }
    const auto highest = static_cast<float>(static_cast<std::underlying_type_t<Enum>>(last));
    return static_cast<Enum>(std::lround(std::clamp(value, 0.0f, highest)));
}

}  // namespace thl::inline THL_ABI_NAMESPACE::dsp::utils
