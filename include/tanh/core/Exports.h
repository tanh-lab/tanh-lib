// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file Exports.h
/// @brief TANH_API — the export decoration of tanh-lib's public API.
///
/// The components are compiled with hidden symbol visibility; TANH_API is the allowlist
/// that marks what a shared component exports (dllexport/dllimport on Windows,
/// visibility("default") elsewhere — see ExportMacros.h), so that nothing else — above
/// all the bundled third-party code (miniaudio, nlohmann::json, moodycamel) — ever
/// appears in its export table. Two definitions steer it, both set by
/// tanh_apply_symbol_policy in CMake:
///
///  - TANH_STATIC: the components are static libraries. Defined PUBLIC, so consumers see
///    it through the CMake package. TANH_API is then empty on every platform: a static
///    archive has no export table of its own — its objects become part of the consumer,
///    and a plugin embedding it must not export tanh-lib's API (dllimport would look for
///    __imp_ stubs an archive never provides; default visibility would leak the API into
///    the plugin's export table).
///  - TANH_BUILDING: defined PRIVATE while compiling a component itself; selects
///    dllexport over dllimport on Windows.

#include <tanh/core/ExportMacros.h>

#if defined(TANH_STATIC)
#define TANH_API
#elif defined(TANH_BUILDING)
#define TANH_API THL_DECL_EXPORT
#else
#define TANH_API THL_DECL_IMPORT
#endif
