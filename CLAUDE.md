# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Git Commits

Never add a `Co-Authored-By` trailer to commit messages.

## Project Overview

tanh-lib is a modular, real-time-safe C++20 audio library with independently buildable components. Namespace: `thl`. Platforms: macOS (12.0+), iOS (14.0+), Android, Linux, Windows.

## Build Commands

```bash
just build                    # Configure + build desktop debug (cmake preset desktop-debug)
just test                     # Build + run all tests via ctest
just test-filter PATTERN      # Run tests matching a pattern
just test-audio               # Run AudioIO tests only
just test-hardware            # Run hardware-dependent tests (requires audio devices)
just format                   # clang-format all source files
just format-check             # Check formatting without modifying
just tidy                     # Run clang-tidy on src/
just tidy-fix                 # Run clang-tidy with auto-fix
just clean                    # Remove all build directories
just build-release            # Build release variant
```

Direct CMake:
```bash
cmake --preset desktop-debug
cmake --build --preset desktop-debug --parallel
ctest --preset desktop-debug
```

Run a single test binary directly:
```bash
./build/desktop/Debug/test/dsp/test_dsp --gtest_filter="TestSuite.TestName"
```

## Architecture

Five library targets with inter-component dependencies:

- **tanh_core** (`src/core/`, `include/tanh/core/`) — Dispatcher (event messaging), Logger (with RT-safe `Logger::rt` path), RCU (lock-free read-copy-update), `LockFreeQueue` (bounded lock-free MPMC), generic buffers, header-only WAV decoder (`WavReader.h`). Foundation for all other components.
- **tanh_state** (`src/state/`, `include/tanh/state/`) — Hierarchical parameter storage with dot-separated paths (e.g. `"oscillator.frequency"`). RCU-protected reads for real-time safety. JSON serialization via nlohmann_json. Depends on core.
- **tanh_dsp** (`src/dsp/`, `include/tanh/dsp/`) — DSP processors (synth, effects, granular, Rings resonator model). All processors inherit `BaseProcessor` with `prepare()`/`process()` interface. Modulation via change points for sample-accurate automation. Depends on core.
- **tanh_modulation** (`src/modulation/`, `include/tanh/modulation/`) — Modulation matrix routing sources (LFO, etc.) to DSP parameters. Change-point-driven sub-blocking. Depends on core, state, dsp.
- **tanh_audio_io** (`src/audio-io/`, `include/tanh/audio-io/`) — Cross-platform audio device I/O over miniaudio. Platform-specific code for iOS (.mm) and Android. Depends on core.

## Naming Conventions

Enforced by `.clang-tidy`:
- Classes/structs/enums: `PascalCase`
- Methods/functions: `snake_case`
- Member variables: `m_` prefix (`m_sample_rate`)
- Constants: `k_` prefix (`k_max_grains`)
- Enum values: `PascalCase`
- Macros: `ALL_CAPS_WITH_UNDERSCORES`
- Folders: `kebab-case`

## Code Style

Configured in `.clang-format` (Google-based): 100-char line limit, 4-space indent, K&R braces.

`.clang-format`, `.clang-tidy`, `.clangd` and every file under `cmake/tanh/` are installed verbatim from a pinned [tanh-tooling](https://github.com/tanh-lab/tanh-tooling) release (`install.sh` families `clang cmake`), and the `tooling-config` CI job fails on any drift from that pin or on a foreign file in `cmake/tanh/`. Never edit these files by hand; changes go to tanh-tooling. To update: run the installer with the new tag (`curl -fsSL https://raw.githubusercontent.com/tanh-lab/tanh-tooling/vX.Y.Z/install.sh | sh -s -- clang cmake`), commit the rewritten files, and bump the `ref` in `.github/workflows/pr-checks.yml` in the same commit. anira pins the same modules and fetches tanh-lib: both repositories must pin the same tag. See README, "Shared tooling".

When running clang-tidy, use multithreading via `run-clang-tidy` (or the `-j` flag) to parallelize across translation units.

## Platform Detection and System Dependencies

`cmake/tanh/platform.cmake` sets `TANH_OPERATING_SYSTEM`, `TANH_BINARY_FORMAT` (ELF/Mach-O/PE/Wasm), `TANH_IOS_PLATFORM` and `TANH_PLATFORM_COMPILE_DEFINITIONS` — exactly one `THL_PLATFORM_*` define (iOS, Android, macOS, Emscripten, Linux, Windows; Emscripten is resolved before the generic `UNIX` branch), which `tanh_core` carries PUBLIC. Branch on `TANH_OPERATING_SYSTEM` / `TANH_BINARY_FORMAT`, never on `APPLE`/`UNIX`/`WIN32`. `tanh_core` must not link platform system libraries by default: the Linux journald sink is opt-in via `TANH_WITH_JOURNALD` (`THL_WITH_JOURNALD`), and the core containers (`Buffer`, `MemoryBlock`, `RingBuffer`) must stay header-only and free of `Logger` so permissively licensed consumers (anira) can embed `tanh::Core` unchanged. Allocation failure in the containers throws `std::bad_alloc`; contract violations `assert`.

## Install and Export

`cmake/install.cmake` installs the built components and the `tanh` CMake package. Exported target names equal the in-tree aliases (`tanh::Core`, `tanh::State`, ...) via `EXPORT_NAME`; `Config.cmake.in` validates requested components against `TANH_EXPORTED_COMPONENTS`. Keep both in sync when adding a component.

## Symbol Policy

Every component gets `tanh_apply_symbol_policy(<target> EXPORT_PREFIX TANH)` and `tanh_set_export_allowlist(<target> NAMESPACE thl)` (loop in the top-level CMakeLists.txt): hidden visibility, `TANH_BUILDING`/`TANH_STATIC` driving `TANH_API` (`include/tanh/core/Exports.h`, a stub over `ExportMacros.h`), and a generated version script / exports list pinning shared components to `thl::`. Consequences: every class or free function that consumers use needs `TANH_API` (missing decoration = link error on every platform, not only Windows); a type thrown or `dynamic_cast` across the boundary needs it for its typeinfo; a `static`/`thread_local` inside an inline function of an undecorated header template is duplicated per DSO (the RCU registry lives in `src/core/RCU.cpp` for that reason). `test/exports` checks the export tables of every component and of a plugin-shaped module in CTest.

## Real-Time Safety

- `process()` methods are marked `TANH_NONBLOCKING_FUNCTION` and must not allocate or block
- Threads must call `ensure_thread_registered()` before RT access to State/StateGroup
- Numeric parameter types (double, float, int, bool) are fully RT-safe; strings may allocate beyond SSO
- `thl::Logger::log/logf/...` are **not** RT-safe. From RT code use `thl::Logger::rt::logf()` / `rt::log()` (lock-free queue, drained by a background thread into the normal sinks). `thl::core::rt_snprintf` is the RT-safe formatter behind it
- Enable RealtimeSanitizer with `-DTANH_WITH_RTSAN=ON` (requires Clang 20+)

## Dependencies (via CMake FetchContent)

- nlohmann_json 3.12.0 (State)
- miniaudio 0.11.24 (AudioIO)
- googletest 1.14.0 (tests)
- googlebenchmark 1.9.1 (benchmarks)

## Rings Reference Fixtures

DSP tests for the Rings resonator compare against reference data. Without fixtures, these tests are skipped. Generate with `./test/dsp/generate_reference_fixtures.sh` (requires SSH access to `tanh-lab/mutable-instrument-api`).
