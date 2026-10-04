# Changelog

All notable changes to tanh-lib are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and the project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- `thl::modulation::ParameterBackend` / `ParameterBinding`: `ModulationMatrix` reads
  parameters through a backend instead of `thl::State`, so a host can bind its own
  parameter store (base-value atomic, gesture flag, `ParameterDefinition`) without
  mirroring into a State. `ModulationMatrix(thl::State&)` keeps working through the
  new `StateParameterBackend`. Modulation now builds with `TANH_BUILD_STATE=OFF`.
  `get_smart_handle` keeps the v0.4.0 error contract on every backend:
  `thl::StateKeyNotFoundException` for an unknown key (a State-backed matrix rethrows
  State's own exception, e.g. `StateGroupNotFoundException`),
  `thl::ParameterTypeMismatchException` for a type mismatch, `std::invalid_argument`
  when modulation is disabled; `tanh/state/Exceptions.h` is installed with Modulation
  when State is off. New `SmartHandle<T>::store_base(T)`: relaxed store into the bound
  base atomic (the counterpart of `ParameterHandle<T>::store()`, no notifications), so
  `ParameterBinding::m_base` holds writable `std::atomic<T>*`. The
  `SmartHandle(ParameterHandle<T>, ResolvedTarget*)` constructor is gone.
  Tests: `ParameterBackend.*`, `StateParameterBackend.*`. Docs: "Modulation with host
  parameters (JUCE)" (`docs/sphinx/host_parameters.md`).
- `thl::RCU::add_reader()` / `read_scope(reader)`: a reader slot owned by the RCU
  instance instead of by a thread, so one logical reader can move between threads
  without registering on each. `ModulationMatrix::audio_read_scope()` (and `process()`)
  use such a slot: a host that renders consecutive blocks on different threads no
  longer allocates and locks (per-thread reader registration) on the audio thread.
  The per-thread path (`read_scope()`, `ensure_thread_registered()`) is unchanged.
  Tests: `RCU.OwnedReaderProtectsAcrossChangingThreads`,
  `ParameterBackend.ProcessOnChangingThreadsNeedsNoRegistration` (aborts under RTSan
  without the slot).
- `thl::dsp::transport::TransportInfo`: trivially copyable per-block musical-time
  snapshot (beat at block start in quarter notes, slope, bpm, time signature, bar
  start, loop, host time, block length) with validity/state flags and the
  discontinuity flags `k_jumped` (with `m_jump_delta_beats`), `k_started`,
  `k_stopped`, `k_tempo_changed` and `k_timeline_reset`; `phase()` (negative-safe),
  `division_boundary_in()`. `ContinuityTracker` derives the flags for every clock
  and absorbs ppq / host-time jitter below a tolerance (2 samples by default).
  `TransportClock` gains `block_info()` and `discontinuities()` (defaulted, so
  existing clocks compile unchanged). Tests: `TransportInfo.*`, `ContinuityTracker.*`,
  `InternalTransportClockInfo.*`. Docs: `docs/sphinx/transport.md`.
- `thl::dsp::transport::HostTransportClock`: a `TransportClock` fed by the plugin
  host's playhead (`set_host_info()` per block) with fallback tempo, time signature,
  play state and a drift-free free-running beat for fields the host omits. Tests:
  `HostTransportClock.*`, including a deterministic simulated host (seek, DAW loop
  wrap, tempo change, start/stop requested mid-block) and an RTSan audio-path test.
- `thl::modulation::XYPad`: UI touches (`touch(id, x, y)`, `release(id)`,
  `release_all()`) become `<prefix>.x` / `.y` / `.active` matrix sources. One
  lock-free queue carries x and y together (same in-block offsets), touch slots are
  allocated in C++ (configurable maximum, cap 16), global pads reduce several
  fingers with `MonoPriority::Last`/`First`, voice-scoped pads map slot = voice, and
  a full queue coalesces moves without losing gate edges. An owner (XY controller)
  can drive it with `process_block(n)` and read `stream()` / `primary()`; the matrix
  outputs then reuse that render. `detail::spread_offset` is shared with
  `InputEventQueue`. Tests: `XYPad.*`, `XYPadMatrix.*` (incl. RTSan audio path).
- Ableton Link behind `TANH_WITH_LINK` (default OFF): the `tanh::Link` component
  with `thl::link::LinkSession` over the Link C++ SDK 4.1 (FetchContent, tag
  `Link-4.1`, recursive submodules) on macOS/Linux/Windows and the official
  LinkKit 4.1.2 release zip (pinned SHA256) on iOS; Android is a configure error.
  `thl::dsp::transport::LinkTransportClock` (in DSP, over the `LinkBackend` seam)
  adds output latency, applies queued tempo/play/seek requests on the audio thread
  with quantized launch, and maps the sample counter to Link time when the host
  gives none. Tests: `LinkTransportClock.*` (scripted fake session, always built),
  `LinkSession.*` (real SDK incl. a two-peer loopback test, with the option).
- `thl::modulation::MotionRecorder`: records one XY pad's primary stream (x, y,
  gate; pre-matrix, so playback never feeds back) on the audio thread into two
  preallocated take buffers (`MotionRecorderConfig::m_max_points`, default 32768,
  576 KiB per recorder) at 200 points/s or a tick-per-beat grid. Free takes (end on
  release; Beats timebase rounded to whole beats while the transport plays) and
  1–16-bar takes (end after exactly one loop, bar-aligned). 5-tap binomial
  smoothing, a raised-cosine seam blend, playback by uniform Catmull-Rom clamped to
  [0, 1] every 32 samples, free-run or hold while stopped, reverse, a live touch
  overriding playback with a 25 ms glide back, glides on transport jumps (from the
  clock's flags, never interpolating across a jump; a running take is never aborted). UI
  commands through a lock-free queue (`arm`, `record`, `disarm`, `play`, `stop`,
  `set_reverse`); finished takes are published by `service()` on the message thread
  through RCU and the audio thread switches to them by take id without a click;
  `ui_snapshot()` packs state, phase and progress into atomics. Driver API for an XY
  controller: `process(TransportInfo, XYPadStream, n)` then `out_x/out_y/out_gate/
  out_live/change_points`; optional matrix sources via `source(XYPadAxis)`.
  `thl::modulation::MotionLane` is the published value type with `sample(phase)` and
  JSON (`to_json`/`from_json`, uint16 x/y, run-length gate, validated, never throws).
  Tests: `MotionLane.*`, `MotionRecorder*.*` (figure-8 acceptance, seam, bar
  alignment, tempo, touch override, handoff, every clock discontinuity, matrix,
  TSan stress, RTSan scenario); benchmark `bm_motion_recorder_playback`. Docs:
  `docs/sphinx/motion_recording.md`.
- `ModulationMatrix::set_routing_enabled(id | source, target, bool)` and the batched
  `set_routings_enabled(span<RoutingEnabled>)`: a disabled routing is fully inert —
  no Additive term, no Replace value, no ReplaceHold hold (a Replace routing at
  depth 0 still writes `src * 0` and wins its target) — and its held state is
  cleared, so re-enabling never revives a stale hold. No schedule rebuild: the audio
  thread reads every flag once per block through a wait-free seqlock, so a batch is
  seen completely or not at all; enable/disable edges flag a change point at offset
  0. `ModulationRouting::m_enabled` (default true) is serialised as
  `"enabled": false` only when disabled, so existing JSON is unchanged. Tests:
  `RoutingEnabled.*` (Additive / Replace / ReplaceHold / multi-Replace / cyclic,
  JSON, concurrent toggling without rebuild, RTSan).
- `thl::TripleBuffer<T>` (`tanh/core/threading/TripleBuffer.h`): wait-free
  single-producer / single-consumer latest-value mailbox for trivially copyable
  frames (`write()` or `write_buffer()` + `publish()`, `read()` / `update()` +
  `latest()`). Tests: `TripleBuffer.*` (incl. a TSan consistency stress and RTSan).
- `thl::modulation::XYController`: one or more XY dots (voices), each with its own
  `XYPad` and `MotionRecorder`, exposed as `<id>.x` / `.y` / `.active` matrix sources
  (`<id>.<v>.x` … with several voices). One driver step per controller and block,
  run from whichever output's `pre_process_block()` comes first (no dependency on the
  matrix's source order), with the block's `TransportInfo` from `set_transport()`
  (or a stand-alone `process_block(t)`); outputs only copy in `process()`. Touch >
  motion, using the recorder's single glide back after a release; x/y stay live once
  a dot has a value so re-enabled routings pick it up at once; Kaoss-style latch;
  `reset()`. Several voices: a touch grabs the nearest enabled dot no other finger
  holds and moves only that voice (`touch_voice()` bypasses selection). Owns its
  routings (`route`, `route_voices`, `unroute`, `set_route_enabled`,
  `set_route_depth`; removed in the destructor). UI: a fixed-size `XYFrame` per
  block through `TripleBuffer` (rate-limited to 240 Hz, immediate on state change,
  skipped while `set_ui_attached(false)`), a drop-oldest live recording trail
  (`drain_trail`) and the recorded path via the recorder's RCU lane and
  `m_path_version` (`read_path`). `XYModeRouter` wires Single/PerEffect routings
  and switches them with one enable batch. Tests: `XYController.*`,
  `XYControllerThreads.*` (TSan), `XYControllerRtsan.*`, `XYControllerState.*`;
  benchmarks `bm_xy_8_controllers`, `bm_xy_matrix_elasticfx`. Docs:
  `docs/sphinx/xy_controller.md`.

### Changed

- `InternalTransportClock`: a tempo change keeps the beat continuous (re-anchored at
  the current position) instead of rescaling the whole sample count, which made the
  beat jump. `Division`, `beats_per_division()` and `division_from_int()` moved to
  `tanh/dsp/transport/TransportInfo.h` (still included by `TransportClock.h`).
  `TransportClock`'s vtable grew (`block_info()`, `discontinuities()`): an ABI change.

### Fixed

- `ContinuityTracker`: a tempo change inside a block (DAW tempo map step or ramp,
  tempo automation, a Link peer's change applied in the past) is a tempo change, not
  a jump. Hosts report one tempo per block, so the predicted block end missed the
  next start by up to `frames · Δbps`, and the 2-sample tolerance flagged
  `k_jumped`; the recorder then shifted a running bar take off the bar grid (0.04
  beats at 2048 samples, 120 → 180 BPM) and a playing lane glided. The window is now
  `[min(0, e) - tol, max(0, e) + tol]` with `e = prev_frames · (bpm - prev_bpm) / (60
  · sr)`: inside it the start snaps and the slope lands on the source's end, and
  `k_tempo_changed` is set. Seeks, loop wraps and realignments still jump. Tests:
  `ContinuityTracker.AbsorbsTempoChangeInsidePreviousBlock`,
  `ContinuityTracker.TempoWindowIsSignedAndBounded`,
  `ContinuityTracker.NoTempoWindowAfterAHeldBlock`, `HostTransportClockTempoMap.*`,
  `LinkTransportClock.PeerTempoChangeInsidePreviousBlockIsNotAJump`,
  `MotionRecorderTempoMap.*` (SimHost gains a sample-accurate tempo map:
  `TempoAt`, `TempoRamp`).
- `MotionRecorder`: the transport clock is the only jump detector. The recorder
  read none of `TransportInfo`'s discontinuity flags and re-detected jumps from the
  lane phase with its own threshold (two lane points), so the two detectors
  disagreed and `XYController::reset()`'s `k_timeline_reset` was ignored. Now
  `k_jumped` / `k_started` re-seek playback (glide if the lane phase moved by more
  than two points), `k_timeline_reset` re-locks without a glide (also restarting the
  stopped free-run), and an unflagged block is continuous. One documented jump rule
  for running takes: a jump never aborts or shifts a take; it continues on its own
  clock and ends after its length; only `prepare()` aborts. Tests:
  `MotionRecorderClockFlags.OnlyTheFlagsDecideAboutAGlide`,
  `XYController.ResetRelocksWithoutGlide`. Docs: `motion_recording.md` ("Clock
  discontinuities", "Jump rule for a running take"), `transport.md`.

### Deprecated

- `SmartHandle<T>::raw_handle()` now returns `std::optional<thl::ParameterHandle<T>>`
  (the State handle for a State-backed matrix, `std::nullopt` for any other backend)
  and is only declared when tanh is built with State (`TANH_STATE_ENABLED`). Kept for
  one release; use `store_base()` / `load_base()`, or `State::get_handle<T>(key)`.

## [0.4.0] - 2026-09-21

### Added

- `thl::core::RingBuffer<T>`: strided block calls, `push_block(channel, data, count,
  stride)`, `pop_block(channel, data, count, stride)` and `peek_past_block(channel,
  data, count, stride)`. They read or write `data[0]`, `data[stride]`, ... and are
  exactly `count` per-sample calls over that run, so one channel of an interleaved
  host buffer (`data = interleaved + c`, `stride = num_channels`, any channel count)
  moves between the host and the ring without a de-interleaved copy in between. A
  strided pop leaves the elements between the strided ones untouched and
  value-initialises what the ring cannot deliver; an oversized strided push keeps the
  tail of the run; `stride == 1` is the existing call (two `std::copy_n` segments).
  None of them allocates. Tests: `RingBufferStrided.*` (differential against the
  per-sample calls for 1, 2, 3 and 6 interleaved channels across the seam).
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

- `thl::RCU`: a version could be reclaimed while a reader was still inside the
  read scope that handed it out. A reader entering a section stores its
  generation and then loads the data pointer; the writer stores the pointer and
  then reads the generations. That store-load handshake needs sequential
  consistency, but used `memory_order_release` / `acquire`, which leave a release
  store free to sit in the core's store buffer while the following load runs —
  so the writer could read generation 0 in `cleanup_safe_versions()`, free the
  version, and leave the reader on freed memory. Seen as an intermittent
  audio-thread SIGSEGV on a freed `ProcessingConfig` in
  `ConcurrentRebuild.RepeatedAddRemoveSingleRouting`, reproducible only on some
  x86-64 hosts (Intel Xeon Platinum 8573C). The four handshake operations are
  now `memory_order_seq_cst`; the reader pays one locked exchange per section
  entry, still wait-free. Leaving a section is unchanged.
- `dsp::granular::GrainProcessorImpl`: volume modulation stepped the output.
  `VoiceParams::m_volume` is a per-sub-block constant and was applied raw, so it
  was the only unsmoothed term in the voice gain (the ADSR already moves per
  sample) — a hard modulation step, such as a square LFO swinging both rails in
  one sample, reached the output as a discontinuity. The voice gain now ramps
  volume over `k_volume_smoothing_duration` (5 ms), seeded to the current level
  in `prepare()` and again at note-on so a voice starts at its level instead of
  sliding up to it.


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

[Unreleased]: https://github.com/tanh-lab/tanh-lib/compare/v0.4.0...HEAD
[0.4.0]: https://github.com/tanh-lab/tanh-lib/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/tanh-lab/tanh-lib/compare/v0.2.0...v0.3.0
