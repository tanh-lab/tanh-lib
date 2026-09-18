# tanh-lib

[![codecov](https://codecov.io/gh/tanh-lab/tanh-lib/branch/main/graph/badge.svg)](https://codecov.io/gh/tanh-lab/tanh-lib)
[![docs](https://github.com/tanh-lab/tanh-lib/actions/workflows/build_docs_and_deploy.yml/badge.svg)](https://tanh-lab.github.io/tanh-lib/)

Modular, real-time-safe C++20 audio library. Six independently buildable
components, all in namespace `thl`:

- **tanh::Core** -- Dispatcher, Logger (with a real-time path), RCU, lock-free
  queue, generic buffers, header-only WAV decoder
- **tanh::State** -- hierarchical parameter storage with RCU lock-free reads and
  JSON serialization
- **tanh::DSP** -- processors, effects, filters, sample player / looper, slicing,
  pitch banks, granular engine, Rings resonator model (also available alone as
  `tanh::Resonator`)
- **tanh::Modulation** -- modulation matrix with change-point sub-blocking
- **tanh::AudioIO** -- audio device I/O over miniaudio (desktop, iOS, Android)
- **tanh::Net** -- verified HTTPS asset delivery (off by default)

Platforms: macOS 12+, iOS 14+, Android, Linux, Windows, WebAssembly.

## Documentation

Getting started, the component overview, the real-time rules, design notes and
the generated API reference live at
[https://tanh-lab.github.io/tanh-lib/](https://tanh-lab.github.io/tanh-lib/),
rebuilt from `main` on every push. Build it locally with `just docs` (needs
Doxygen, Graphviz and Python 3).

## Build

Requires CMake 3.25+ and a C++20 compiler.

```bash
cmake --preset desktop-debug          # or: cmake . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build --preset desktop-debug --parallel
ctest --preset desktop-debug
```

`just build`, `just test`, `just format`, `just tidy`, `just docs` wrap the
everyday commands (`just --list`). `CMakePresets.json` also has iOS, Android,
Windows, WebAssembly and sanitizer presets.

### CMake options

| Option | Default | Description |
|---|---|---|
| `BUILD_SHARED_LIBS` | ON | Build as shared libraries |
| `TANH_BUILD_CORE` | ON | Build the Core component |
| `TANH_BUILD_STATE` | ON | Build the State component |
| `TANH_BUILD_DSP` | ON | Build the DSP (and Resonator) component |
| `TANH_BUILD_MODULATION` | ON | Build the Modulation component |
| `TANH_BUILD_AUDIO_IO` | ON | Build the AudioIO component |
| `TANH_BUILD_NET` | OFF | Build the Net component (links a platform HTTP stack) |
| `TANH_WITH_TESTS` | ON | Build test targets |
| `TANH_WITH_EXAMPLES` | ON | Build the example projects (iOS audio-io app) |
| `TANH_WITH_DOCS` | OFF | Add the `sphinx-docs` target (Doxygen + Sphinx) |
| `TANH_WITH_INSTALL` | ON | Add install targets and the `tanh` CMake package |
| `TANH_WITH_PACKAGING` | OFF | Add CPack targets (top-level builds only) |
| `TANH_WITH_RTSAN` / `_ASAN` / `_USAN` / `_TSAN` / `_MSAN` / `_LSAN` | OFF | Enable the respective sanitizer (RTSan needs Clang 20+) |
| `TANH_WITH_JOURNALD` | OFF | Linux: route platform logs to systemd-journald |
| `TANH_LOG_COMPILED_MAX_LEVEL` | AUTO | Most verbose log level compiled in (1 Error .. 4 Debug) |

### Consume

```cmake
# as a subdirectory / FetchContent
set(TANH_WITH_TESTS OFF)
set(TANH_WITH_EXAMPLES OFF)
add_subdirectory(modules/tanh-lib)

# or from an installed prefix
find_package(tanh REQUIRED COMPONENTS Core DSP)   # Core State DSP Resonator Modulation AudioIO Net

target_link_libraries(app PRIVATE tanh::Core tanh::DSP)
```

The exported target names match the in-tree aliases, so consumers link the same
`tanh::<Component>` either way. See the
[Getting Started](https://tanh-lab.github.io/tanh-lib/getting_started.html) page
for installing, the Rings reference fixtures and running the tests.

## Contributing

Formatting, linting and the CMake modules under `cmake/tanh/` come verbatim from
a pinned [tanh-tooling](https://github.com/tanh-lab/tanh-tooling) release and
are not edited here; every component is built with hidden visibility and
`TANH_API` decides what a shared component exports. Both are described in the
[Contributing](https://tanh-lab.github.io/tanh-lib/contributing.html) and
[Symbol Visibility](https://tanh-lab.github.io/tanh-lib/symbol_visibility.html)
pages.

## License

tanh-lib is licensed per component:

- **Apache-2.0** — the **core component** (`tanh::Core`): generic containers
  (`Buffer<T>`, `BufferView`, `RingBuffer<T>`, `MemoryBlock<T>`), the header-only
  WAV decoder (`read_wav`), `Logger`
  (including the real-time `Logger::rt` queues and drain threads, and `rt_snprintf`),
  `Thread` (an OS thread with a requested scheduling class),
  `Dispatcher`, threading utilities (RCU, `LockFreeQueue`), and small helpers — everything under
  `include/tanh/core/`, `include/tanh/core.h`, `src/core/`, `src/core.cpp` —
  plus the binary-data CMake helpers (`cmake/tanh/binary-data.cmake`,
  `cmake/tanh/bin2cpp.cmake`, from tanh-tooling). One target, one license: linking `tanh::Core` pulls
  in Apache-2.0 code only, so permissively licensed projects (e.g.
  [anira](https://github.com/anira-project/anira)) can depend on it. These
  files carry an `SPDX-License-Identifier: Apache-2.0` header; the license
  text is in [`LICENSE-APACHE-2.0`](LICENSE-APACHE-2.0).

- **AGPL-3.0** — all other components (`state`, `dsp`, `resonator`,
  `modulation`, `audio-io`) and files (see [`LICENSE`](LICENSE)).
