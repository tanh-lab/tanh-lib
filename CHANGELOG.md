# Changelog

All notable changes to tanh-lib are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and the project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- Sample playback as standalone components, usable without the granular voice
  or any parameter system (design notes: `docs/sphinx/sampler.md`, `slicing.md`,
  `pitch.md`):
  - `dsp::sampler::SamplePlayer`: a varispeed player / looper over
    `SampleView`s, with Start / End / Loop markers or an exact region. End before
    Start plays backwards, and moving a marker never moves the audible head. A
    loop wraps through a crossfade before End (or after the wrap when Loop has no
    room behind it), equal-power, or equal-gain when both ends sit on zero
    crossings. Other discontinuities (source switch, retrigger, one-shot end) are
    crossfaded through a pool of outgoing heads. Tuning lives in
    `PlayerSettings`; `render()` takes a span of alternative sources, and
    `sources_changed()` must be called when the host loads new audio (snapped
    markers are cached against a source's address and length).
  - Its building blocks: `SampleView` with `read_clamped` / `read_wrapped`,
    `LoopRegion`, `LoopMarkers` (minimum span, zero-crossing snap, cache),
    `ZeroCrossing.h`, and `LoopCrossfade.h` (`FadeCurve`, `plan_loop_fade`).
  - `dsp::slicing::SliceMap`: up to 64 slices in frames, trivially copyable for an
    audio-thread handoff, rescaled for sources of another length; `SliceSteps.h`
    for the slice-space mapping and `region_from_steps`;
    `valid_normalized_bounds` for host edits.
  - `dsp::slicing::TransientSlicer`: offline band-flux transient detection that
    picks N slices, refines boundaries against the audio and falls back to the
    grid; every threshold is in `Settings`.
  - `dsp::pitch::PitchBank`: pitch-shifted, equal-length copies of a sample, one
    slot per semitone, built across worker threads with Signalsmith Stretch.
    `build()` / `shift()` take the sample rate explicitly and refuse a
    non-positive one.
    Signalsmith is fetched and linked privately, and `test/exports` forbids it
    in the export table.
  - `dsp::utils::mixdown`: mono average of a buffer, for offline analysis.
- `dsp::granular::GrainEngine` is usable standalone: it takes a `GrainParams`
  block, a `HeadMode` (`Spray` / `Scan`), a span of sources and planar output,
  and reports to an optional `GrainVisualizer`.

- `Net` component (`tanh::Net`, `TANH_BUILD_NET`, **off by default**) — delivery
  of versioned file sets over HTTPS, for shipping model or sample packs that are
  too large to bundle.
  - `thl::net::HttpClient` — GET to a file or a string, `Range` resume, progress
    with cancellation, and cancel from another thread. Backends are
    platform-native (`NSURLSession` today) rather than a vendored TLS stack, so
    certificate validation uses the OS trust store and there is no CA bundle to
    ship or rotate inside a released plugin. Platforms without a backend report
    `HttpStatus::Unsupported`; `HttpClient::supported()` lets a caller check once
    rather than per transfer. Windows, Linux and Android backends are not
    written yet.
  - `thl::net::Sha256` — FIPS 180-4 in plain C++, streaming and whole-file, with
    `matches_hex` that rejects malformed input rather than trusting it. Plain C++
    rather than OS crypto so the component needs one platform matrix (HTTP) and
    not two.
  - `thl::net::AssetStore` — verified, atomic install of a caller-described file
    set into `<root>/<id>/<version>/`. The completion marker is written last, so
    a directory without it is a partial install that `is_installed()` refuses and
    `prune_partial()` clears. Ids, versions and file names from a server are
    validated against path traversal. A failed install resumes without
    re-fetching files that already verify.
  - The component knows nothing about manifests, models or packs: the caller
    supplies `{id, version, files:[{url, name, size, sha256}]}`.
  - Off by default so nothing embedding `tanh::Core` inherits a network stack.

  Tested against a loopback HTTP server rather than a live endpoint — the
  failures worth covering (a well-formed response carrying wrong bytes, a
  declared length that is never delivered, a server ignoring `Range`,
  cancellation mid-transfer) cannot be produced on demand against a real bucket.
  One `DISABLED_` test fetches over real TLS when run with
  `--gtest_also_run_disabled_tests`.

### Fixed

- `dsp::granular::GrainProcessorImpl`: volume modulation stepped the output.
  `VoiceParams::m_volume` is a per-sub-block constant and was applied raw, so it
  was the only unsmoothed term in the voice gain (the ADSR already moves per
  sample) — a hard modulation step, such as a square LFO swinging both rails in
  one sample, reached the output as a discontinuity. The voice gain now ramps
  volume over `k_volume_smoothing_duration` (5 ms), seeded to the current level
  in `prepare()` and again at note-on so a voice starts at its level instead of
  sliding up to it.

### Changed

- **Breaking**, `dsp::granular`: the voice internals were split into the
  components above. Hosts that subclass `GrainProcessorImpl` must change the
  following:
  - `SlicerEnabled`, `LoopEnabled` and `LoopSnap` moved to the end of the
    `Parameter` enum. Hosts that serve parameters by name are unaffected; tables
    indexed by the enum's numeric value must be reordered.
  - `read_slice_map()` takes a `dsp::slicing::SliceMap`, which is frame-based,
    instead of the old normalised 16-slice `granular::SliceMap`. Build it with
    `SliceMap::from_normalized(bounds, total_frames)`.
  - `prepare()`'s `samples_per_block` is now used: it sizes the player's scratch.
  - `granular::SamplePlayer`, `SampleReader`, `SampleRegion` and `SliceMap` are
    gone; use `sampler::SamplePlayer`, `sampler::SampleView`,
    `sampler::LoopRegion` and `slicing::SliceMap`.
  - `GrainEngine` no longer takes a `SampleReader` and `VoiceParams`: see the
    new API above.
  - `HeadPolicy` reads `HeadInputs`.
  - The `k_player_*` and `k_*_varispeed` constants moved into
    `sampler::PlayerSettings`.

  The Sample head now renders source channels and the voice applies the channel
  mode afterwards (`channel_mixer::mix_head`). Grain output is bit-identical
  before and after the split; Sample-mode output differs by at most 1.2e-7 of
  full scale, from floating-point reordering.
- `TANH_WITH_DOCS` now does something — it adds the `sphinx-docs` target — and
  therefore defaults to **OFF** (it was ON and inert). A docs-enabled configure
  requires Doxygen and Python 3; consumers that already set it OFF are
  unaffected.
- `dsp::granular::SamplePlayer`: the equal-power crossfade reads a table instead
  of calling `sin`/`cos` per frame — a block fading the live head plus four
  outgoing tails wanted five transcendentals per frame. Because
  `cos(t * pi/2) == sin((1 - t) * pi/2)`, one quarter-sine table sized to the
  fade length in `prepare()` serves both directions, indexed straight by the
  integer fade counter: exact at every index, so the crossfade values are
  unchanged bit for bit.
- `dsp::granular::GrainProcessorImpl`: the voice gain pass skips the mode-fade
  ramp when the fade is already parked on its target, which is every block
  outside a mode switch. Same output, two fewer compares and a store per frame.

### Added

- Documentation: a Doxygen → Breathe → Sphinx site under `docs/` (the same
  pipeline as anira), published to https://tanh-lab.github.io/tanh-lib/ by the
  new `build_docs_and_deploy` workflow on every push to `main`. `just docs`
  builds it locally. The API reference is generated from `include/tanh/`; the
  README's design notes (symbol visibility, `InputEventQueue` event spreading,
  the Android Bluetooth SCO notes) moved into the docs and the README is now a
  short entry point. Doxygen comments in the audio-io and state headers were
  corrected on the way (`@param` names that no longer matched the parameters,
  `@section` labels reused across classes, a `@copydetails` that copied the
  wrong overload).
- `dsp::granular`: reverse playback from the markers alone — End before Start
  makes Start the entry and End the exit, so the Sample head runs backwards
  (re-entering at Loop) and Loop-mode grains scan and play backwards.
  `SampleRegion` keeps ascending bounds plus `m_reverse` and mirrors only the
  read position (`physical()`), so the loop floor, crossfade pool and
  region-shrink logic are unchanged. Grain reads wrap below zero.

- `dsp::utils::MorphWindow`: a bank of eight grain windows (Rectangle,
  Trapezoid, Half cosine, Triangle, Hann, Gaussian, Narrow, Impulse) morphed
  by a continuous shape value — integers land exactly on a shape, fractions
  blend the neighbours — and skewed by a tilt in [-1, 1] that moves the peak
  by warping time around it. One shared instance, 512-point tables, peak 1,
  silent end points. `GrainProcessorImpl` reads two new parameters,
  `GrainWindowShape` and `GrainWindowTilt` (subclasses must serve them); a
  grain keeps the window it was born with.

- `dsp::granular::GrainProcessorImpl`: engine modes — `EngineMode::GranularPosition`
  (grains sprayed around a parked `Position` with `Spray` / `Tilt`),
  `EngineMode::GranularLoop` (previous scan behaviour) and `EngineMode::Sample`
  (one continuous varispeed head, no grains). New parameters `EngineModeParam`,
  `Position`, `Spray`, `Tilt` extend the subclass `Parameter` enum; subclasses
  must serve them from `get_parameter_*`. Mode switches on a sounding voice fade
  through zero (`k_mode_change_fade_duration`).
- Sample mode in `MonoToStereo` puts half the mono sum in each channel, the
  level a centred grain has under the linear pan law, so a mode switch no
  longer steps the level by 6 dB.
- Sample mode crossfades every head discontinuity — loop wrap, pitch-bank
  switch and retrigger — with a small pool of outgoing heads, so a second
  discontinuity inside a fade never hard-cuts.

### Changed

- `dsp::granular::GrainEngine` render loop: bank pointers and pan gains are
  resolved once per block per grain, one channel-mode kernel is chosen per
  block, and the window comes from the new `dsp::utils::MorphWindow`.
  Roughly half the CPU at a full pool; `test/dsp/benchmark_Granular.cpp`
  measures it. Idle voices no longer scan the pool and report to the
  visualiser every block.
- Position mode: a Spray window past either sample edge clips to the edge
  instead of wrapping to the other end — what the waveform band shows.

- `dsp::granular` split into components: `GrainProcessorImpl` is now the
  per-voice facade (parameter snapshot `VoiceParams`, master ADSR, mode fade)
  over a pre-allocated `GrainEngine` (grain pool + scheduler, told where to
  start grains by a `HeadPolicy`: `LoopScanHead` / `PositionSprayHead`) and a
  `SamplePlayer` (the Sample-mode head). `SampleReader`, `SampleRegion` and
  `channel_mixer` are the shared, header-only helpers. Constants and enums
  moved to `GranularTypes.h` (still reachable through `GrainProcessor.h`).
  The subclass contract (`Parameter` enum, the three `get_parameter_*` hooks)
  is unchanged. Behaviour differences: a Sustain change now retunes the decay
  slope at once (the per-setter path left the rate stale until another
  envelope parameter moved); lingering grains are always reported finished on
  reset / silence; `prepare()` starts the voice from silence.

### Fixed

- `ModulationMatrix` / `RCU`: data race between a schedule rebuild and the audio
  thread. `RCU::update` is copy-on-write, so it deep-copies the live value —
  including each `ResolvedRouting`'s `m_held_voice_values` and per-voice
  freshness vectors, which the audio thread writes in place through the const
  routing it is processing. `rebuild_schedule_with_lock` assigns every
  `ProcessingConfig` member anyway, so that copy was discarded immediately.
  New `RCU::replace()` publishes a freshly built value without reading the one
  the readers hold; the rebuild now uses it. `update()` is unchanged and
  documents when not to use it. Caught by TSan via
  `ConcurrentRebuild.PolyReplaceContentionChurnDoesNotCrash` — the existing
  concurrency tests route Additive only and never touch the held state.
- `ModulationMatrix`: crash on the audio thread when a second Replace routing is
  added to a polyphonic target. A rebuild publishes each target's fresh
  `VoiceBuffers` one step *before* the new `ProcessingConfig`, so an in-flight
  audio block still running the old routings can load a buffer whose
  `m_has_replace_priority` has just gone false -> true. That sends a routing
  resolved as single-Replace down the multi-Replace branch of
  `apply_replace_sample_voice`, where it indexes per-voice freshness vectors its
  own rebuild left unsized -- a null dereference on the audio thread. The
  vectors are now sized for every polyphonic Replace routing (contended or not),
  and the multi-Replace branch bounds-checks the voice index. Reproduced by
  `ConcurrentRebuild.PolyReplaceContentionChurnDoesNotCrash`.

## [0.3.0] - 2026-09-03

First release with a changelog: earlier releases (v0.1.0, v0.2.0) are described only by their tag messages (`git tag -n1 v0.1.0 v0.2.0`).

### Added

- `Logger`: configurable platform-sink identity. `LoggerConfig::m_platform_tag`
  (Android logcat tag, journald `SYSLOG_IDENTIFIER`), `m_platform_subsystem` and
  `m_platform_category` (Apple `os_log`) name what the platform sink files records
  under, so an embedder (anira) shows up under its own name in `adb logcat -s`,
  Console.app and `log stream`. Defaults `"thl"`, `"thl"`, `"logger"` keep the
  previous output; an empty string selects the default. Set them before the first
  record: a later `set_config()` applies to records dispatched after it returns, and
  on Apple platforms creates a new `os_log_t` (which the system never releases).
- `Logger`: per-record flags. `LogRecord::m_flags` travels unchanged from the
  emitting site to the sinks (the console, file and platform sinks ignore it; the
  callback sink sees it). New overloads take the flags right after the level:
  `log_with_source(level, flags, source, group, message)`,
  `logf(level, flags, group, fmt, ...)`, `rt::logf`/`rt::log(level, flags, ...)` and
  `rt::Queue::logf`/`vlogf`/`log(level, flags, ...)`; the existing signatures pass
  0. Reserved bits: `k_flag_realtime` (1, set by `rt::Queue::drain()` on every
  record it dispatches) and `k_flag_contract_violation` (2, only ever set by the
  caller). The real-time producers stay allocation- and lock-free (one more
  `uint32_t` in the fixed-size record).
- `Logger`: `LogRecord::m_dropped_before`. A `rt::Queue::drain()` pass takes the
  queue's drop counter before it pops and puts the count on the first record it
  dispatches; every other record carries 0, so summing the field over all records
  received counts every drop exactly once. `format_plain()` appends
  `[N real-time log message(s) dropped before this record]` and `format_logfmt()`
  a `dropped_before=N` field to a record that carries a count.

- CMake: `TANH_LOG_COMPILED_MAX_LEVEL` (`AUTO`, or `1`..`4`) chooses the most verbose log
  level compiled into the call sites. `AUTO`, the default, keeps the historical rule
  (Error only in Release builds, every level otherwise); an embedder that wants its runtime
  level to be the only filter in a shipped build sets `4` as a plain variable before the
  fetch.

### Changed

- `Logger`: the synthetic drop warning of `rt::Queue::drain()` (level Warning, source
  `rt`, group `thl.logger`) is only dispatched when a pass has no record to carry the
  count. Its message is now `real-time log queue overflowed` with the count in
  `m_dropped_before` (and in the rendered suffix), instead of
  `N real-time log message(s) dropped (queue full)` after every pass with drops.

[Unreleased]: https://github.com/tanh-lab/tanh-lib/compare/v0.3.0...HEAD
[0.3.0]: https://github.com/tanh-lab/tanh-lib/compare/v0.2.0...v0.3.0
