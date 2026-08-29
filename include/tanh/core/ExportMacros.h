// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file ExportMacros.h
/// @brief The platform switch behind every tanh-lab library's export macro.
///
/// A library's `<lib>/.../Exports.h` defines its `<LIB>_API` as a three-way selector
/// on the definitions set by tanh_apply_symbol_policy (cmake/tanh/symbol-policy.cmake):
///
///     #if defined(MYLIB_STATIC)        // static library: no decoration, ever
///     #define MYLIB_API
///     #elif defined(MYLIB_BUILDING)    // set only while compiling the library itself
///     #define MYLIB_API THL_DECL_EXPORT
///     #else
///     #define MYLIB_API THL_DECL_IMPORT
///     #endif
///
/// so the platform-specific spelling lives here once:
///  - Windows: __declspec(dllexport) / __declspec(dllimport) — PE exports nothing
///    without the former, and an importing translation unit needs the latter.
///  - GCC/Clang: visibility("default") on both sides on purpose. The libraries are
///    compiled with -fvisibility=hidden, so this attribute is what puts the API into the
///    export table; inline members and other vague-linkage entities a consumer
///    instantiates from the headers keep default visibility as well and are coalesced
///    with the library's copies at load time instead of becoming private duplicates.
///  - Anything else: empty.
/// THL_DECL_HIDDEN forces a symbol out of the export table regardless of the class it
/// belongs to.
///
/// MSVC's C4251 ("class needs to have dll-interface") is inherent to exported classes
/// holding std:: members and is suppressed by the CMake policy (/wd4251), not here.

#if defined(_WIN32)
#define THL_DECL_EXPORT __declspec(dllexport)
#define THL_DECL_IMPORT __declspec(dllimport)
#define THL_DECL_HIDDEN
#elif defined(__GNUC__) || defined(__clang__)
#define THL_DECL_EXPORT __attribute__((visibility("default")))
#define THL_DECL_IMPORT __attribute__((visibility("default")))
#define THL_DECL_HIDDEN __attribute__((visibility("hidden")))
#else
#define THL_DECL_EXPORT
#define THL_DECL_IMPORT
#define THL_DECL_HIDDEN
#endif
