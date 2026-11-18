// SPDX-License-Identifier: Apache-2.0
#pragma once

/// Platform-portable std::numbers constants.
/// Older Apple Clang ships the `<numbers>` header without exposing std::numbers
/// in C++20 mode. Use the standard header whenever the library advertises it
/// (__cpp_lib_math_constants), so this header and `<numbers>` can be included
/// together; provide a manual fallback only where it is missing.
#include <version>

#if defined(__cpp_lib_math_constants)
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
