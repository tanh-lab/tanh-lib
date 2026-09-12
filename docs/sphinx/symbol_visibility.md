# Symbol visibility


Every component is compiled with hidden symbol visibility; `TANH_API` (`include/tanh/core/Exports.h`, self-contained: it holds the dllexport/dllimport/visibility switch itself; `tanh/core/ExportMacros.h` is only a deprecated forwarding shim kept for one release) is the allowlist of what a shared component exports, and a shared component additionally pins its export table to namespace `thl` at link time (`tanh_set_export_allowlist` in `cmake/tanh/symbol-policy.cmake`). So `libtanh_audio_io.so` exports `thl::` and nothing else — not miniaudio's `ma_*`, not `nlohmann::json`, not `moodycamel`, not libstdc++ instantiations — and cannot be interposed against a host application that ships its own copy of any of those. A static component is compiled without export decoration (`TANH_STATIC`, defined for consumers through the CMake package), so a plugin that embeds it exports nothing of tanh-lib either. `test/exports` verifies both shapes in CTest on Linux, macOS and Windows.

Plugins should embed the **static** components. Sharing one `libtanh_*.so` between independently shipped plugins is only sound while every version carrying the same SONAME is ABI-compatible — the loader binds the first copy it sees to every later plugin — and every 0.x release has `SOVERSION 0` without such a promise.

## What this means when adding API

- Every class or free function a consumer uses needs `TANH_API`. A missing
  decoration is a link error on every platform, not only on Windows.
- A type that is thrown or `dynamic_cast` across the library boundary needs
  `TANH_API` for its typeinfo.
- A `static` or `thread_local` inside an inline function of an undecorated
  header template is duplicated per shared object. The RCU registry lives in
  `src/core/RCU.cpp` for that reason.
- `test/exports` checks the export table of every component, and of a
  plugin-shaped module that embeds the static components, in CTest.
