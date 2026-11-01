# Symbol visibility


Every component is compiled with hidden symbol visibility; `TANH_API` (`include/tanh/core/Exports.h`, self-contained: it holds the dllexport/dllimport/visibility switch itself; `tanh/core/ExportMacros.h` is only a deprecated forwarding shim kept for one release) is the allowlist of what a shared component exports, and a shared component additionally pins its export table to namespace `thl` at link time (`tanh_set_export_allowlist` in `cmake/tanh/symbol-policy.cmake`). So `libtanh_audio_io.so` exports `thl::` and nothing else — not miniaudio's `ma_*`, not `nlohmann::json`, not `moodycamel`, not libstdc++ instantiations — and cannot be interposed against a host application that ships its own copy of any of those. A static component is compiled without export decoration (`TANH_STATIC`, defined for consumers through the CMake package), so a plugin that embeds it exports nothing of tanh-lib either. `test/exports` verifies both shapes in CTest on Linux, macOS and Windows.

Plugins should embed the **static** components. Sharing one `libtanh_*.so` between independently shipped plugins is only sound while every version carrying the same SONAME is ABI-compatible — the loader binds the first copy it sees to every later plugin — and every 0.x release has `SOVERSION 0` without such a promise.

## The ABI namespace

Hidden visibility keeps a shared component's internals out of the host's export table, but it does not keep two copies of tanh-lib apart that end up in one link: a plugin's static tanh-lib and the host's own, or an embedder's private core (anira compiles one into libanira) next to the application's tanh-lib. Static archives are resolved by name, so without more the linker would pick one copy of `thl::Logger::set_level` for both sides; with different versions that is an ABI mismatch, with the same version the two sides share the logger's state.

So every declaration lives in an inline namespace: each opener reads `namespace thl::inline THL_ABI_NAMESPACE::core {`, and `thl::core::Logger` is `thl::v0_4::core::Logger` to the compiler and the linker (`_ZN3thl4v0_4…` on ELF and Mach-O, `…@v0_4@thl@@` on PE). Code keeps spelling `thl::…`; only the mangled names, debugger output and log symbolication show the namespace. Templates and inline functions in the headers are mangled with it too, so header-only code of two copies never merges either.

- **The value** is `TANH_ABI_NAMESPACE`: `AUTO` (the default) is `v<major>_<minor>` of the version from git, so every minor release changes every mangled name (normal for 0.x: consumers rebuild against a new release anyway, and two releases can now share a process). Any C++ identifier can be set instead.
- **One source for it:** the core's `tanh_add_core_library()` writes `#define THL_ABI_NAMESPACE <value>` into a generated `tanh/core/AbiNamespace.h` in the library's build tree, which is in the core's public include directories and is installed with it. Every component compiles against the core's header, and so does every consumer, in-tree or from the installed package; a header that does not match the library fails to link instead of binding silently.
- **Objective-C** class names are global to the process, so the namespace is pasted into them: the Net component's download delegate is `TanhDownloadDelegate_<ns>`.
- **The remaining limit: miniaudio.** AudioIO compiles miniaudio's C sources as C++ (`src/audio-io/miniaudio_impl.cpp`), so miniaudio's functions are C++ symbols at global scope (`ma_device_init(...)`), outside any namespace. In a shared AudioIO they are hidden and stay inside it. Two copies of AudioIO in one static link (a host's and a plugin's tanh-lib, both with AudioIO) still define the same `ma_*` symbols, and the linker resolves both copies' miniaudio calls to one of them or reports duplicate symbols, as it did for all of tanh-lib before the namespace. The core and the other components are not affected; anira's private core contains no miniaudio. The symbol check of `test/abi-namespace` allows the `ma_` prefix for this reason.
- **Consequences for consumers:** code that forward-declares a tanh-lib type itself (`namespace thl { class State; }`) declares a second, different `thl::State`: where the tanh-lib header is seen too, the name is ambiguous (a compile error), and where it is not, a function taking it no longer links. Include the tanh-lib header instead.

## A private core beside tanh_core

`cmake/core-library.cmake` defines the core component, and tanh-lib's own `tanh_core` is defined through it:

```cmake
include(<tanh-lib>/cmake/core-library.cmake)
tanh_add_core_library(<target> SOURCE_DIR <tanh-lib> ABI_NAMESPACE <ns>
                      [STATIC | SHARED] [LOG_COMPILED_MAX_LEVEL <AUTO | 1..4>]
                      [WITH_JOURNALD] [ALIAS <name>])
```

It adds the core sources, include directories, public definitions, platform libraries, the symbol policy and the version properties, and generates the target's own `tanh/core/AbiNamespace.h`. An embedder that needs a core no host can interfere with fetches tanh-lib under its own FetchContent name, without running tanh-lib's top-level `CMakeLists.txt`, and builds its own copy in its own namespace:

```cmake
FetchContent_Declare(anira_tanh_lib
    GIT_REPOSITORY https://github.com/tanh-lab/tanh-lib.git
    GIT_TAG <commit>
    SOURCE_SUBDIR anira-uses-the-core-only)   # populate only
FetchContent_MakeAvailable(anira_tanh_lib)
include(${anira_tanh_lib_SOURCE_DIR}/cmake/core-library.cmake)
tanh_add_core_library(anira_tanh_core
    SOURCE_DIR ${anira_tanh_lib_SOURCE_DIR}
    ABI_NAMESPACE anira
    STATIC
    LOG_COMPILED_MAX_LEVEL 4)
```

A parent that fetches tanh-lib itself (`FetchContent_Declare(tanh-lib …)`, namespace `v0_4`) and the embedder's `anira_tanh_core` (namespace `anira`) then coexist in one build and one process, in any link mode: they share no symbol and no state. The file has no include guard on purpose: each caller includes the copy of the tree it builds right before the call, so two tanh-lib trees in one build each use their own function. The function leaves sanitizers and install rules to the caller.

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
- Open every namespace as `namespace thl::inline THL_ABI_NAMESPACE[::…] {`,
  forward declarations included, and include `<tanh/core/AbiNamespace.h>`
  directly. `scripts/abi-namespace.py` rewrites plain openers (re-run it after a
  rebase that brings in new files, then clang-format what it lists).
- `test/abi-namespace` checks it in CTest on Linux, macOS and Windows:
  `abi_namespace_symbols_<library>` reads every symbol a library defines (nm,
  dumpbin) and fails on one of `thl` outside the namespace or on a function of
  tanh-lib at global scope (what the toolchain defines there is exempt: reserved
  names, libc++'s ABI-tagged functions such as Apple's `::abs` overloads, the C++
  overloads of the C library that MSVC's `<cmath>` defines globally),
  `abi_namespace_sources` runs the script's `--check`, and `test_abi_namespace`
  links a second core library (`tanh_core_b`, namespace `test_b`) beside
  `tanh_core` and shows that their loggers keep separate levels, configurations
  and callbacks.
