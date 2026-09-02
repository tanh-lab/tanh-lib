// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file ExportMacros.h
/// @brief Deprecated forwarding shim — the platform switch now lives in Exports.h.
///
/// TANH_API is self-contained since Exports.h inlined the dllexport/dllimport/
/// visibility("default") spelling; every tanh-lab library owns its own export selector
/// (anira ships include/anira/abi/export.h). This header is kept for one release so
/// that a remaining user — anira's 2.x line still includes it — keeps compiling; new
/// code includes <tanh/core/Exports.h> directly. The THL_DECL_* definitions below
/// exist for source compatibility only.

#include <tanh/core/Exports.h>

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
