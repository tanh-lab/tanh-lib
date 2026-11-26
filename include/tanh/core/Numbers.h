// SPDX-License-Identifier: Apache-2.0
#pragma once

/// The std::numbers constants. Every standard library that builds tanh-lib's C++20 code has them
/// (__cpp_lib_math_constants); where one does not, this header stops the build and names the
/// missing feature instead of declaring the constants in namespace std itself.
#include <version>

#if !defined(__cpp_lib_math_constants)
#error "tanh-lib needs the C++20 std::numbers constants (__cpp_lib_math_constants)"
#endif

#include <numbers>  // IWYU pragma: export
