#pragma once

/// Platform-portable std::numbers constants.
/// Apple Clang ships the <numbers> header but does not expose std::numbers
/// in C++20 mode.  Detect Apple Clang via __apple_build_version__ and
/// provide a manual fallback.
#if __has_include(<numbers>) && !defined(__apple_build_version__)
#include <numbers>
#else

namespace std::numbers {  // NOLINT(cert-dcl58-cpp)

template <typename T>
inline constexpr T pi_v = static_cast<T>(3.14159265358979323846L);

template <typename T>
inline constexpr T inv_pi_v = static_cast<T>(0.31830988618379067154L);

template <typename T>
inline constexpr T ln2_v = static_cast<T>(0.69314718055994530942L);

inline constexpr double pi = pi_v<double>;
inline constexpr double inv_pi = inv_pi_v<double>;
inline constexpr double ln2 = ln2_v<double>;

}  // namespace std::numbers

#endif
