# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

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

- **tanh_core** (`src/core/`, `include/tanh/core/`) — Dispatcher (event messaging), Logger, RCU (lock-free read-copy-update). Foundation for all other components.
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

## Real-Time Safety

- `process()` methods are marked `TANH_NONBLOCKING_FUNCTION` and must not allocate or block
- Threads must call `ensure_thread_registered()` before RT access to State/StateGroup
- Numeric parameter types (double, float, int, bool) are fully RT-safe; strings may allocate beyond SSO
- Enable RealtimeSanitizer with `-DTANH_WITH_RTSAN=ON` (requires Clang 20+)

## Dependencies (via CMake FetchContent)

- nlohmann_json 3.12.0 (State)
- miniaudio 0.11.24 (AudioIO)
- googletest 1.14.0 (tests)
- googlebenchmark 1.9.1 (benchmarks)

## Rings Reference Fixtures

DSP tests for the Rings resonator compare against reference data. Without fixtures, these tests are skipped. Generate with `./test/dsp/generate_reference_fixtures.sh` (requires SSH access to `tanh-lab/mutable-instrument-api`).
