#pragma once

/// @file Exports.h
/// @brief DLL export/import macros for Windows shared-library builds.
///
/// When building tanh-lib as a shared library on Windows, each public symbol
/// must be annotated with __declspec(dllexport) (when compiling the library)
/// or __declspec(dllimport) (when consuming the library). On GCC/Clang the
/// macro expands to a default-visibility attribute so that -fvisibility=hidden
/// builds still export the public API.
///
/// CMake sets TANH_BUILDING_SHARED (PRIVATE) on each library target and
/// TANH_SHARED (PUBLIC) when BUILD_SHARED_LIBS is ON.

#if defined(_WIN32) || defined(_WIN64)
#ifdef TANH_BUILDING_SHARED
#define TANH_API __declspec(dllexport)
#elif defined(TANH_SHARED)
#define TANH_API __declspec(dllimport)
#else
#define TANH_API
#endif
// Suppress MSVC warning C4251 (class needs dll-interface for data members)
#pragma warning(disable : 4251)
#elif defined(__GNUC__) || defined(__clang__)
#define TANH_API __attribute__((visibility("default")))
#else
#define TANH_API
#endif
