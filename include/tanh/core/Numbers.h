#pragma once

/// Platform-portable std::numbers constants.
/// Apple Clang ships the <numbers> header but does not expose std::numbers
/// in C++20 mode.  Detect Apple Clang via __apple_build_version__ and
/// provide a manual fallback.
#if __has_include(<numbers>) && !defined(__apple_build_version__)
#include <numbers>  // IWYU pragma: export
#else

namespace std::numbers {  // NOLINT(cert-dcl58-cpp)

template <typename T>
inline constexpr T pi_v = static_cast<T>(3.14159265358979323846L);

template <typename T>
inline constexpr T inv_pi_v = static_cast<T>(0.31830988618379067154L);

template <typename T>
inline constexpr T e_v = static_cast<T>(2.71828182845904523536L);

template <typename T>
inline constexpr T ln2_v = static_cast<T>(0.69314718055994530942L);

template <typename T>
inline constexpr T sqrt2_v = static_cast<T>(1.41421356237309504880L);

template <typename T>
inline constexpr T sqrt3_v = static_cast<T>(1.73205080756887729353L);

inline constexpr double pi = pi_v<double>;
inline constexpr double e = e_v<double>;
inline constexpr double inv_pi = inv_pi_v<double>;
inline constexpr double ln2 = ln2_v<double>;
inline constexpr double sqrt2 = sqrt2_v<double>;
inline constexpr double sqrt3 = sqrt3_v<double>;

}  // namespace std::numbers

#endif
