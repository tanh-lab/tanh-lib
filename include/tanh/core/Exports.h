// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file Exports.h
/// @brief TANH_API — the export decoration of tanh-lib's public API.
///
/// The components are compiled with hidden symbol visibility; TANH_API is the allowlist
/// that marks what a shared component exports (dllexport/dllimport on Windows,
/// visibility("default") elsewhere), so that nothing else — above all the bundled
/// third-party code (miniaudio, nlohmann::json, moodycamel) — ever appears in its
/// export table. Two definitions steer it, both set by tanh_apply_symbol_policy in CMake
/// (cmake/tanh/symbol-policy.cmake):
///
///  - TANH_STATIC: the components are static libraries. Defined PUBLIC, so consumers see
///    it through the CMake package. TANH_API is then empty on every platform: a static
///    archive has no export table of its own — its objects become part of the consumer,
///    and a plugin embedding it must not export tanh-lib's API (dllimport would look for
///    __imp_ stubs an archive never provides; default visibility would leak the API into
///    the plugin's export table).
///  - TANH_BUILDING: defined PRIVATE while compiling a component itself; selects
///    dllexport over dllimport on Windows.
///
/// The platform spelling is inlined here so the header is self-contained — every
/// tanh-lab library owns its own selector, and a consumer needs no second header:
///  - Windows: __declspec(dllexport) / __declspec(dllimport) — PE exports nothing
///    without the former, and an importing translation unit needs the latter.
///  - GCC/Clang: visibility("default") on both sides on purpose. The libraries are
///    compiled with -fvisibility=hidden, so this attribute is what puts the API into the
///    export table; inline members and other vague-linkage entities a consumer
///    instantiates from the headers keep default visibility as well and are coalesced
///    with the library's copies at load time instead of becoming private duplicates.
///  - Anything else: empty.
///
/// MSVC's C4251 ("class needs to have dll-interface") is inherent to exported classes
/// holding std:: members and is suppressed by the CMake policy (/wd4251), not here.

#if defined(TANH_STATIC)
#define TANH_API
#elif defined(TANH_BUILDING)
#if defined(_WIN32)
#define TANH_API __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define TANH_API __attribute__((visibility("default")))
#else
#define TANH_API
#endif
#else
#if defined(_WIN32)
#define TANH_API __declspec(dllimport)
#elif defined(__GNUC__) || defined(__clang__)
#define TANH_API __attribute__((visibility("default")))
#else
#define TANH_API
#endif
#endif
