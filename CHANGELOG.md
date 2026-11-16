# Changelog

All notable changes to tanh-lib are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and the project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- Host hand-over, as a DAW's Touch mode: a Replace / ReplaceHold routing with
  `ModulationRouting::m_share_with_host` (`XYRouteOptions::m_share_with_host`) shares
  its target with the host. The last of the two writers to become active owns it;
  when the owner stops, the other takes over if active, else the value stays. Every
  hand-over crossfades (`HostShareTimes`: take 20 ms, return 50 ms, host hold 300 ms;
  `ModulationMatrix::set_host_share_times()`), flagging change points at least every
  32 samples while it moves. Host activity: `ParameterBinding::m_host_change_stamp`
  (a counter the backend bumps on every host write) and the gesture flag.
  `ModulationSource::output_activity()` separates "driven" from "value valid" (an XY
  pad's x / y: touch, playback or latch); `"share_with_host": true` is serialised.
  Tests: `HostShare.*`. Docs: `host_parameters.md`.
- `tanh::Parameters`: what a parameter is (`ParameterDefinition`, `Range`,
  `ModulationScope`, `parameter_type_of<T>()`) and the lookup errors
  (`ParameterNotFoundException`, `ParameterTypeMismatchException`), shared by State
  and Modulation in `tanh/parameters/`. AGPL-3.0; built whenever State or Modulation
  is (no option). Install test: `test/install-modulation`.
- `thl::modulation::ParameterBackend`: `ModulationMatrix` reads parameters through a
  backend (`find(key)` returns definition, base atomic and gesture flag) instead of
  `thl::State`, so a host binds its own parameter store without mirroring it; a key
  is looked up once. An unknown key throws through `throw_not_found()`
  (`ParameterNotFoundException`, or State's subclasses for a State-backed matrix).
  `ModulationMatrix(thl::State&)` keeps working through `StateParameterBackend` and,
  with `state()`, is only declared when tanh is built with State. Modulation builds
  with `TANH_BUILD_STATE=OFF` (CI job `modulation_without_state`). Tests:
  `ParameterBackend.*`, `StateParameterBackend.*`. Docs: `host_parameters.md`.
- `ModulationMatrix::set_routing_enabled()` / `set_routings_enabled()`: a disabled
  routing writes nothing, Replace included (depth 0 does not silence it), and drops
  its hold. No rebuild: the audio thread reads the flags once per block through a
  wait-free seqlock, so a batch lands whole. Edges and the first block after a
  rebuild flag offset 0; a zero-sample block keeps the edge. `"enabled": false` is
  serialised only when disabled. Tests: `RoutingEnabled.*`.
- `thl::modulation::BlockProcessor`: work a `ModulationMatrix` runs once per block
  before any source (`add_block_processor()`), given a `BlockContext` with the block
  length and the transport passed to `process()` / `process_with_scope()`.
- `thl::dsp::transport::TransportInfo`: trivially copyable per-block snapshot with
  the flags `k_jumped`, `k_started`, `k_stopped`, `k_tempo_changed` and
  `k_timeline_reset`, derived for every clock by a `ContinuityTracker` that absorbs
  jitter and in-block tempo changes, so consumers react to flags and never compare
  beats. `HostTransportClock` follows the host's playhead with fallbacks for omitted
  fields. `HostTimeFilter` maps a callback's sample time to host time. Tests:
  `TransportInfo.*`, `ContinuityTracker.*`, `TransportClock.*`, `HostTransportClock*.*`,
  `HostTimeFilter.*`. Docs: `transport.md`.
- `tanh::Link` (`TANH_BUILD_LINK`, off by default for its licence):
  `thl::link::LinkSession` over the Link 4.1 SDK (pinned commit) on macOS, Linux and
  Windows and LinkKit 4.1.2 (pinned SHA256, CMake 3.28+) on iOS, with
  `capabilities()` for what each SDK lets the app control. `LinkTransportClock` sits
  in DSP behind the `LinkBackend` seam (`timeline_backend()`), takes the callback's
  host time through `set_host_time_us()`, compensates output latency and applies
  requests at the output time with launches quantized to the session's quantum
  (`set_quantum()`). Link Audio: `audio_channels()`, `set_audio_input()` and the
  `audio_backend()` seam (received buffers in, our output out, sent with the clock's
  capture of the same block); `LinkAudioReceiver` plays the received channel on the
  local beat grid a latency behind. Built and tested in every desktop and iOS
  preset. Tests: `LinkTransportClock.*` (fake session), `LinkAudioReceiver.*`,
  `LinkSession.*`, `LinkAudioCodec.*`, and with the label `link-peers` (excluded by
  the presets) `LinkPeers.*`, `LinkPeersAudio.*`. Docs: `transport.md`.
- `thl::modulation::MotionRecorder` and `MotionLane`: record an XY gesture on the
  audio thread into preallocated memory and loop it back synced to the transport,
  with touch override and a glide back. Every take is a Beats loop of whole bars:
  `Free` records one gesture from the touch to the release, drops its still ends and
  is stretched onto the nearest of 1, 2, 4, 8 or 16 bars; `BarsN` is a looper over N
  bars from the loop line that records until `disarm()`, starts as what plays,
  overwrites only where the finger is down and hands the loop over at every release,
  so it plays smoothed and transformed between touches (one undo step per take). The
  recorder is a facade over tested parts: `detail::MotionTakeRecorder`,
  `detail::MotionLaneStore` (hand-over slots, RCU publication, undo),
  `detail::MotionLanePlayer` (phase, playback length, reverse, jumps, render ramp) and
  `detail::MotionCompositor` (touch over playback over hold). The transport flags are
  the only jump detector; a jump never aborts a running take. UI commands are
  `[[nodiscard]]` (false when the 32-entry queue is full); `service()` publishes
  takes; lanes save as validated JSON. `set_playback_length(BarsN)` plays any lane
  stretched onto N bars from a bar line. A take that keeps nothing leaves the lane
  that played. The published lane stays raw, seam included (`MotionLane::m_seam`,
  JSON `seam`); what plays (`played_lane()`, also `make_played_curve()` for any lane)
  closes the seam over 0.3 beat (150 ms), widening to the whole loop as
  `set_smoothing()` goes to 1, then runs x and y through a zero-phase Gaussian of
  sigma = amount² × half a beat (250 ms for Seconds lanes).
  `set_loop_end(LoopEnd::Jump)` keeps the end-to-start step as a hard edge and smooths
  without crossing it. Trail points carry the take's loop position
  (`XYPathPoint::m_position`) and the finger (`m_live`), so a UI can draw a looper as
  it is overwritten. Tests: `MotionTakeRecorder.*`, `MotionLaneStore.*`,
  `MotionLanePlayer.*`, `MotionCompositor.*`, `MotionLane.*`, `MotionRecorder*.*`,
  `MotionLaneProcessing.*`. Docs: `motion_recording.md`.
- `thl::modulation::MotionTransform`: the application's path over playback (shapes,
  scaling, rotation), stateless, with its setting as one 64-bit word
  (`MotionRecorder::set_transform()`, `set_transform_settings()`,
  `transform_version()`); it moves the lane's points, or loops alone over the playback
  length, and can hold the gate open. `render_motion_path()` samples what plays for
  drawing. Tests: `MotionLanePlayer.Transform*`,
  `MotionRecorderOverwrite.LiftedLooperPlaysSmoothedAndTransformed`. Docs:
  `motion_recording.md`.
- `thl::modulation::XYController`: XY dots (voices), each with touch input and a
  recorder, as `<id>.x` / `.y` / `.active` matrix sources (`XYOutput`). A
  `BlockProcessor` of the matrix, it renders once per block with the matrix's
  transport (free-running without one), owns its routings and publishes an `XYFrame`
  per block, plus a recording trail and the recorded path. `for_each_recorder()`
  reaches every voice's recorder. `XYRouteOptions::m_skip_during_gesture` leaves a
  target alone while its knob is held (a host gesture). Tests: `XYController*.*`,
  `XYPad.*`. Docs: `xy_controller.md`.
- Timestamped touch input: `XYController::touch()` / `touch_voice()` / `release()`
  take an optional `time_ns`, and `set_block_time(now_ns)` once per block places them
  at their own sample, `m_input_delay_ms` late (default one prepared block + 5 ms);
  late touches land at offset 0, later ones wait. x / y ramp linearly between timed
  moves (change point every `m_render_interval` samples); down, up and finger
  switches stay sharp. Untimed touches, or a block without a block time, spread as
  before. Tests: `XYControllerInput.*`, `XYPad.Timed*`.
- Core: `thl::core::uptime_ns()` (`Clock.h`), the uptime clock of UITouch, NSEvent
  and host times on Apple platforms, `steady_clock` elsewhere; `thl::TripleBuffer<T>`,
  a wait-free SPSC latest-value mailbox; `thl::SeqLock`, the sequence counter of a
  wait-free seqlock over atomics; `RCU::add_reader()` / `read_scope(reader)`, a reader
  slot (`RCU<T>::Reader`) owned by the RCU instance, not by a thread, so a host
  rendering on changing threads no longer allocates and locks on the audio thread
  (`ModulationMatrix::audio_read_scope()` and `process()` use one). Tests:
  `TripleBuffer.*`, `SeqLock.*`, `RCU.OwnedReaderProtectsAcrossChangingThreads`.
- `thl::dsp::analysis::SpectrumAnalyser`: the curve of a spectrum analyser display, on top of
  `StftAnalyser`, for a reader thread. Resolution (FFT 1024 to 8192, 75 % overlap), points spaced
  on a log axis (256 from 20 Hz to 20 kHz by default), channels averaged in power; without
  smoothing a point is its band's largest bin (a sine keeps its level), with fractional-octave
  smoothing its band's mean power; tilt in dB per octave around 1 kHz (4.5 by default), speed
  (rises at once, falls at most so many dB per second), freeze (holds the maximum), floor.
  `read_from(SampleProbe&)` drains a probe of the same channel count and starts the frames
  over after a discontinuity. A tilt change moves the held curve at once, a smoothing change
  starts it over; a maximum above Nyquist is clamped to it; a rejected `prepare()` leaves it
  unprepared. One thread for everything (the curve is a view). A full-scale sine reads 0 dB at
  tilt 0. Tests: `SpectrumAnalyser.*`.
- `thl::graph::Graph::add_probe<Probe>(node, ProbePoint, args...)` / `remove_probe(ProbeId)` /
  `probe(ProbeId)`: a probe watches a node's input (its sources mixed) or output (after its
  processor and wet mix), `graph_input()`'s output or `graph_output()`'s input, with the latency
  of that point; it never changes the signal or the graph's latency. Live: no commit, no fade.
  The graph owns its probes (so one probe can never watch two places): it prepares them when
  added and with every `prepare()`, resets them with `reset()`; when a newly committed version
  starts, the probes of its nodes, of the nodes that stopped running and of `graph_output()`
  start over (`graph_input()`'s see a continuous signal and go on), and `remove_probe()` / `remove_node()` delete them once the audio thread no longer
  uses them (at most the block in progress). The returned pointer is for reading. Up to
  `k_max_probes_per_point` per point. Tests: `GraphProbes.*`, including probes added, used and
  removed while the audio thread runs.
- `thl::dsp::probe`: probes watch a signal without changing it. `ProbeBase` gets every block
  read-only through `process(block, latency_samples)` (skipped while inactive,
  `set_active()`); it is not a `BaseProcessor` and never a graph node. `LevelProbe`: peak per
  channel (the highest since the reader last took it, `take_peak()`) and RMS over a one-pole
  window (300 ms by default), as atomics. `SampleProbe`: copies every block into an
  `SpscRingBuffer` for one reader thread (scope, analyser). Wherever the stream is not
  continuous (a `reset()`, the probe coming back from inactive, samples dropped because the
  reader fell behind), `read()` skips what came before and reports a discontinuity, so the
  reader never joins samples across a gap. Readers must not read during `prepare()` (hosts call
  it on the main thread). Tests: `LevelProbe.*`, `Probe.*`, `SampleProbe.*`.
- `thl::core::SpscRingBuffer<T>` (`core/threading/SpscRingBuffer.h`): a multi-channel
  lock-free single-producer / single-consumer ring of samples, e.g. from the audio thread to an
  analyser. The writer appends blocks and never waits: what does not fit is dropped and counted
  (`dropped()`); the reader reads what has arrived, oldest first, or only the newest
  (`read_latest()`), or skips. Capacity rounded up to a power of two; real-time safe on both
  sides (two atomic positions, no locks, no allocation after `prepare()`). Tests:
  `SpscRingBuffer.*`, including one writer and one reader side by side.
- `thl::dsp::fft::StftAnalyser`: the analysis half of a short-time Fourier transform. `push()`
  takes samples in chunks of any size; every hop, once an FFT size has arrived, each channel's
  newest frame is windowed (Hann, or 4-term Blackman-Harris for -92 dB sidelobes), transformed
  (`RealFft`) and handed to the virtual `process_spectrum(channel, bins)`, scaled so a sine of
  amplitude A reads A at its bin (0 dB for full scale). `reset()` drops what was collected, so no
  frame spans a discontinuity. Allocation-free after `prepare()`. Tests: `StftAnalyser.*`.
- `thl::dsp::fft::RealFft`: the FFT of a real signal of a power-of-two size (32 to 65536),
  unscaled forward (as `numpy.fft.rfft`: N / 2 + 1 bins, a sine of amplitude A reads
  A · N / 2), inverse scaled by 1 / N, real-time safe after `prepare()`. The backend is a
  build option, `TANH_FFT_BACKEND` (`cmake/fft-backend.cmake`): `pffft` (default, every
  platform; marton78/pffft v1.1.0 compiled in, BSD-like FFTPACK licence, NEON/SSE) or
  `accelerate` (macOS, iOS: Apple's vDSP). Both give the same bins. Tests: `RealFft.*`
  (against the DFT by its definition, round trip, Parseval), run on either backend.
- `thl::dsp::utils` helpers, real-time safe: `Decibels.h` (`db_to_gain()`, `gain_to_db()`,
  `power_to_db()`, float or double, floored at `k_min_db` = -120 dB or a given floor),
  `TimeConstant.h` (`ms_to_samples()`, `one_pole_pole()`: exp(-1 / samples), 0 for no time)
  and `ParameterValue.h` (`ParameterRange`, `sanitised()`: NaN to a fallback, the rest
  clamped; `to_enum()`). `LimiterImpl`, `EqDesign`, `SvfCoefficients::magnitude_db()` and
  the dynamics blocks use them. Tests: `Decibels.*`, `TimeConstant.*`, `ParameterValue.*`.
- `thl::dsp::dynamics::CompressorImpl`: a compressor and expander on the dynamics
  building blocks. Downward and upward compression and expansion (`Mode`), threshold, ratio
  (infinity limits or gates), knee, range; peak or RMS detector, stereo link (linked, average,
  dual mono), sidechain high-pass; attack, hold, release, auto release; lookahead up to 20 ms,
  reported as `latency_samples()` (the gain moves before the delayed audio); makeup, auto
  makeup, and an internal mix for parallel compression. `process_with_sidechain()` keys it by
  an external signal. Meters: `gain_change_db()`, `input_peak_db()`, `output_peak_db()`.
  Parameters through `get_parameter_float()`, read at every block position; their defaults
  and ranges in `k_parameter_ranges` (a NaN reads as the default). A link change hands the
  gain over, so no channel jumps to an old gain. Tests: `Compressor.*`.
- `thl::dsp::dynamics::LookaheadLimiterImpl`: a brickwall lookahead limiter. The gain each
  sample needs goes through a minimum over the lookahead, a release in dB and a moving
  average over the lookahead, the audio delayed by the lookahead minus one: no output sample
  above the ceiling, the gain alone gets it there (a hard clip catches rounding). Ceiling,
  input gain, lookahead up to 10 ms, release, link (linked or dual mono), true peak (4x,
  97-tap interpolator, +12 samples of latency; holds the ceiling for content up to 0.44 fs).
  `latency_samples()` reports it. Meters: `gain_reduction_db()`, `input_peak_db()`,
  `output_peak_db()`. Defaults and ranges in `k_parameter_ranges` (a NaN reads as the
  default); a link change hands the gain over. Tests: `LookaheadLimiter.*`.
- `thl::dsp::dynamics` (header-only, `include/tanh/dsp/dynamics/`): the building blocks
  of a dynamics processor. `GainComputer`, the static curve in dB for the four modes
  (`DynamicsMode`: downward and upward compression, downward and upward expansion) with
  threshold, ratio (infinity limits or gates), quadratic soft knee and range;
  `LevelDetector`, a peak or one-pole RMS level as a power (a non-finite sample reads as
  silence; a mode change clears the window); `StereoLink.h`, `link_powers()`
  (`StereoLink::Linked`, `Average`; `DualMono` links nothing);
  `GainSmoother`, attack and release in the log domain (attack away from 0 dB, in either
  direction), hold, and an auto release with a slow second stage (switched on, it starts
  from the current gain). Tests: `GainComputer.*`,
  `LevelDetector.*`, `GainSmoother.*`.

### Changed

- `TransportClock` keeps the block itself: a subclass implements `compute_block()` and
  `on_prepare()`, and `prepare()`, `begin_block()`, `end_block()`, `block_info()` (now a
  reference), `beat_at_sample()`, `division_in_block()` and the tempo, signature and
  play getters are non-virtual in the base. `begin_block()` takes only the frame
  count; Link's host time goes to `LinkTransportClock::set_host_time_us()`.
  `sample_position()` counts samples played since `prepare()` on every clock.
  `Division` moved to `TransportInfo.h`. `InternalTransportClock`: a tempo change
  keeps the beat continuous. An API and ABI change for subclasses.
- `ParameterDefinitions.h` and `ModulationScope.h` moved from `tanh/state/` to
  `tanh/parameters/` (the old headers forward, deprecated for one release), and
  `ParameterTypeMismatchException` moved to `tanh/parameters/ParameterExceptions.h`.
  `StateKeyNotFoundException` and `StateGroupNotFoundException` derive from
  `ParameterNotFoundException`. State links `tanh::Parameters`.
- `ModulationMatrix::process()` / `process_with_scope()` take the block's
  `TransportInfo` (optional) for the block processors, and `process()` requires
  `num_samples <= samples_per_block` (it wrote past its buffers): debug builds
  assert, release builds clamp.
- `SmartHandle<T>` reads the base value through the backend's atomic: it is built
  from `(std::atomic<T>*, ResolvedTarget*)`, and the metadata accessors and
  `load_normalized()` assert on a handle without a target.
- `ModulationMatrix::prepare()` needs audio stopped, as a host's `prepareToPlay`
  does: it re-prepares every source and block processor and resets the sample
  counter. A same-size `prepare()` during audio was documented as safe but raced
  that state. Sources and block processors added before the first `prepare()` are
  prepared by it, not at once with a default rate and size.
- `tanh/core/Numbers.h` includes `<numbers>` wherever the standard library has it
  (`__cpp_lib_math_constants`), so it can be included next to `<numbers>`.
- `thl::core::LockFreeQueue` and `SpscRingBuffer` keep their writer's and reader's positions
  `k_cache_line_size` (128 bytes, `core/threading/CacheLine.h`) apart instead of 64: Apple
  Silicon's cache lines are 128 bytes, so 64 still let them share one. `sizeof(LockFreeQueue)`
  grows by up to 3 · 64 bytes. `SpscRingBuffer::read_latest()` takes its window from one snapshot
  of the writer's position.
- `thl::dsp::utils::BandSplitter` reads crossover parameters itself: the virtual
  `get_crossover_parameter_float()` (non-finite by default: the crossover keeps what
  `set_frequency()` gave it) and `read_crossover_parameters()`, which `MultibandLimiterImpl`
  and `StereoWidenerImpl` now call instead of each repeating it. Overrides keep working.

### Fixed

- Clocks: a tempo change inside a block, or a Link peer's dated within 50 ms, is
  `k_tempo_changed`, not `k_jumped`; a Link stop dated ahead lands in its block; the
  first realignment within 1.5 s of a Link epoch change is the `k_timeline_reset`;
  `HostTransportClock` drops a seek made while the host supplies the beat and
  trusts its play flag only with tempo, beat or time signature. Tests:
  `HostTransportClockTempoMap.*`, `LinkTransportClock.JoinRealignmentAfterTheEpochResets`,
  `HostTransportClock.SeekWhileHostSuppliesBeatIsDropped`.
- Clocks: a block that continues the previous one starts exactly at its end, so a
  division boundary on a block edge counts once (it could count twice or not at
  all with a host's sample-derived ppq); the time signature is set in one atomic,
  never seen half updated; a seek or tempo request is taken once (it could apply
  in two blocks); `InternalTransportClock::prepare()` keeps the beat at a new sample
  rate. Tests: `HostTransportClock.DivisionsOnBlockEdgesCountOnce`,
  `InternalTransportClock.{TimeSignatureIsNeverTorn,SeekAppliesOnce,PrepareKeepsTheBeatAtANewSampleRate}`.
- `XYController`: the trail keeps its spacing across blocks shorter than the trail
  interval; a reset that closes a latched gate adds a change point at sample 0.
  Tests: `XYFramePublisher.TrailKeepsItsSpacingAcrossShortBlocks`,
  `XYController.LatchKeepsTheGateOpenUntilTurnedOff`.
- Link clock: our own play or seek right after an epoch change is a plain jump,
  not the join's `k_timeline_reset`, and ends the wait for it; the host-time filter
  starts over after host times, so a stall while they arrived does not offset it.
  Tests: `LinkTransportClock.{OwnSeekAfterTheEpochIsNotAReset,FilterStartsOverWhenHostTimesStop}`.
- RCU: read scopes on one thread or reader slot nest; closing an inner one freed
  the outer one's version (e.g. `audio_read_scope()` around `ModulationMatrix::process()`).
  Test: `RCU.NestedReadScopesKeepTheOuterVersion`.
- `XYController::set_block_time()` holds for one `matrix.process()`: documented that
  a host splitting its block sets it per chunk.
- `XYController`: a repeated `prepare()` re-prepares the recorders; a take published
  by `service()` during a block no longer switches that block to the older lane it
  still reads. Tests: `MotionRecorderThreads.ConcurrentServiceKeepsTheHandoffSilent`.
- `thl::dsp::utils::LimiterImpl`: a NaN threshold, attack or release made its gain, and so
  its output, NaN for good (and with it `MultibandLimiterImpl`'s bands). A NaN now keeps the
  parameter's last value; the threshold is clamped to [-120, 24] dB, the times to
  [0.01, 60000] ms. `process()` is `TANH_NONBLOCKING_FUNCTION`. Test:
  `Limiter.NanParametersKeepTheirLastValue`.

### Removed

- `SmartHandle<T>::raw_handle()`: a backend has no `ParameterHandle`. Use
  `State::get_handle<T>(key)`; hosts write base values through their own API.
- The `host_time_micros` parameter of `TransportClock::begin_block()` (see Changed).


## [0.5.0] - 2026-11-01

### Added

- `tanh_add_core_library()` (`cmake/core-library.cmake`): the core component as a library
  of the calling build, `tanh_add_core_library(<target> SOURCE_DIR <tanh-lib> ABI_NAMESPACE
  <ns> [STATIC | SHARED] [LOG_COMPILED_MAX_LEVEL <AUTO | 1..4>] [WITH_JOURNALD] [ALIAS
  <name>])`, with the sources, include directories, public definitions, platform libraries,
  symbol policy and version properties of the core; tanh-lib's own `tanh_core` is defined
  through it. An embedder that must not share a core with its host (anira) fetches tanh-lib
  under its own FetchContent name, populate-only, includes that tree's
  `cmake/core-library.cmake` and builds a private core in a namespace of its own, e.g.
  `tanh_add_core_library(anira_tanh_core ... ABI_NAMESPACE anira STATIC)`: the host's
  `tanh_core` and the private core coexist in one build and one process. The file has no
  include guard; include the copy of the tree you build right before the call. Sanitizers and
  install rules stay with the caller. See `docs/sphinx/symbol_visibility.md`.
- `TANH_ABI_NAMESPACE` (default `AUTO`, which is `v<major>_<minor>` of the version from git):
  the inline namespace of every declaration. `tanh_add_core_library()` writes it as
  `THL_ABI_NAMESPACE` into a generated `tanh/core/AbiNamespace.h` in the library's build tree,
  which is in the core's public include directories and is installed with the core, so every
  consumer compiles against the value the library was built with.
- `scripts/abi-namespace.py`: rewrites every `namespace thl...` opener and closing comment to
  the ABI namespace and adds the include; idempotent, `--check` for CI. Re-run it after a
  rebase that brings in new files.
- `test/abi-namespace` (CTest, Linux, macOS, Windows): `abi_namespace_symbols_<library>`
  checks with nm / dumpbin that every symbol a library defines in `thl` lies in its ABI
  namespace and none of tanh-lib's lies at global scope (the toolchain's own, such as
  libc++'s ABI-tagged `::abs`, MSVC's global `<cmath>` overloads or the sanitizers'
  `__rtsan` helpers, are exempt);
  `test_abi_namespace` links a second core library (`tanh_core_b`, namespace `test_b`)
  beside `tanh_core` and shows separate logger levels, configurations and callbacks;
  `abi_namespace_sources` runs the script's `--check`. The install consumer prints the
  installed namespace.
- `thl::dsp::fx::StereoWidenerImpl`: a multiband stereo widener. Mid and side are
  split by Linkwitz-Riley crossovers (`BandSplitter`), each band's side scaled by
  its width (0 mono, 1 unchanged, up to 2), the mid only through the crossovers'
  all-passes, so mid and side stay in phase. Mono compatible whatever the widths
  (L + R is the mid signal through the all-passes); two bands with band 0 at
  width 0 is mono bass below the crossover. No latency; widths glide over 20 ms;
  `process_with_dry()` gives the input through the same all-passes, phase-matched.
  Widths through `get_band_parameter_float()`, crossovers through
  `get_crossover_parameter_float()` (or `set_frequency()`), read at every block
  position. Anything but stereo passes unchanged. Tests: `StereoWidener.*`.
- `tanh::Graph` (`TANH_BUILD_GRAPH`, off by default, on in every preset but wasm; by
  @jstolberg): `thl::graph::Graph`, a `BaseProcessor` of `BaseProcessor` nodes. Edits on the
  control thread take effect at `commit()`, handed to the audio thread with a fade; nested
  graphs, per-node input mix and wet mix, `collect_garbage()`. Latency compensation: the
  faster inputs of every merge and a node's dry/bypass path are delayed, the graph holds
  silence after a commit until every delay is ready. It never pads: its output has the least
  latency its nodes allow (`max_latency_samples()` only sizes the delays), and
  `committed_latency_samples()` gives the latency the committed version will have, for the
  host right after `commit()` (a node whose maximum changed since the last commit counts at it
  until it runs). A node whose latency rises above the maximum it had at the last commit (or a
  nested graph that committed on its own) is compensated at that maximum until the graph
  containing it commits. A bypassed node coming back is `reset()`. Merges sum their sources by
  default (`InputMix::Sum`, or `Average`). Per node, `set_mix_law()` chooses how the wet mix
  sets the dry and wet gains (`MixLaw::Linear`, the default, `ConstantPower`, `Compromise`
  at -4.5 dB, `Parallel`, `DryHeld`), live, a change crossfading over 20 ms. Tests: `test_graph`
  (including edits and commits against a running audio thread), `benchmark_graph`.
- `BaseProcessor::latency_samples()` / `max_latency_samples()` / `set_latency_samples()` /
  `set_max_latency_samples()`: a processor reports its latency (by @jstolberg).
- `thl::dsp::utils::DelayProcessor`: an integer delay that crossfades linearly when the delay
  changes, and can keep its history without changing its input (by @jstolberg). Tests:
  `DelayProcessor.*`.
- `BaseProcessor::process_modulated()` takes the buffer's position in the block
  (`modulation_offset`, default 0), so a caller that runs a block in parts, such as a graph,
  has its nodes read modulation at the block position.
- `BaseProcessor::reset()`: clears what a processor holds from the audio it processed, keeping
  its settings; real-time safe, does nothing by default. `LimiterImpl`, `ParametricEqImpl`,
  `StereoFDN` and `DelayProcessor` implement it.
- `thl::dsp::filter::ParametricEq`: a parametric equaliser of up to 8 bands in series for any
  number of channels, each band a Peak, Notch, LowShelf, HighShelf, LowCut or HighCut
  (12 dB/octave). One trapezoidal state-variable filter (Cytomic) per band and channel. Every
  change of a band or of the design ramps its coefficients over 10 ms, so automation and a
  dragged band do not click; a flat or disabled band costs nothing, also when a field it does not
  use changes. Out-of-range and non-finite settings are clamped or replaced; `prepare()` keeps
  the bands. `response_curve()` and `log_frequencies()` give the settled curve to plot, from
  the caller's settings, on any thread. Real-time safe after `prepare()`. Tests:
  `ParametricEq.*`.
- `thl::dsp::filter::ParametricEqImpl`: `ParametricEq` as a `BaseProcessor`, its parameters
  read through `get_parameter_float()` (the design) and `get_band_parameter_float()` (type,
  frequency, gain, Q, enabled per band), so State and Modulation drive it. Tests:
  `ParametricEqImpl.*`.
- `EqDesign.h`: `EqBand`, `design_band()` and `analog_response()`. `EqDesign::Matched` (the
  default) follows the analogue prototype up to Nyquist: poles by impulse invariance, zeros
  fitted to the prototype's magnitude (Vicanek, "Matched Second Order Digital Filters", 2016,
  for the peak and the cuts; the shelves fitted at DC, their frequency and Nyquist up to Q 2,
  bilinear above, where the fit would miss a resonant shelf's bump). `EqDesign::Bilinear` gives
  the RBJ cookbook's curves. Tests:
  `EqDesign.*`.
- `SvfCoefficients.h`: `SvfCoefficients` (Cytomic's g, k, m0 to m2, with the exact response
  and the conversion to and from `BiquadCoefficients`), `SvfState`, `SvfTick` and `SvfRamp`
  (click-free coefficient ramps), header-only, for any filter built on the trapezoidal SVF. The
  coefficients, states and per-sample gains are double: low bands at high sample rates stay at
  the float output's own noise floor.
  Tests: `SvfCoefficients.*`.
- `ConstellationReverbImpl::set_shimmer_position()`: where the shimmer pitch-shifts
  the tank. `Feedback` (the default, unchanged) shifts each half's output, so the
  shimmer is heard after a round of decay and builds with a long decay; `Input`
  shifts each half's input, the fresh input and the feedback together, so it is
  heard on the first pass, also with a short decay. Tests:
  `ConstellationReverb.*`.
- `thl::core::BufferOperations`: header-only static helpers on the samples of
  buffer views, `copy(destination, source)` and `add(destination, source)`, in
  every channel. `source` is a `ConstBufferView`, so writable views and
  `Buffer`s work too. Both views must have the same channels and samples
  (asserted) and must not overlap. Real-time safe. `BufferView` itself stays a
  pure view. Tests: `BufferOperations*`.
- `dsp::filter::Butterworth`: header-only, per-sample low-, high- or all-pass
  (`Butterworth::Response`) of order 1–4: a 1st-order TPT section and
  trapezoidal SVF sections (`SvfTick`), states in double, so a low cutoff at a
  high sample rate stays at the float output's own noise floor. The static
  `prewarp(f, fs)` limits the cutoff to [1 Hz, `k_max_frequency_ratio` (0.497)
  · fs], a non-finite one to 1 Hz; `process(x, g)` takes the prewarped gain, so
  filters at the same frequency share one `tan()`. Real-time safe after
  `prepare()`. Tests: `Butterworth.*`.
- `dsp::filter::LinkwitzRiley`: crossover of even order 2–8, two Butterworths of
  half the order per band. `process_low()` and `process_high()` can be called
  separately, also with `g`. LR2 and LR6 invert the high band, so low + high is
  an all-pass at every order. Tests: `LinkwitzRiley.*`.
- `dsp::utils::SmoothedValue::is_smoothing()`: whether a glide is in progress.
- `dsp::utils::BandSplitter`: an abstract `BaseProcessor` that splits a signal
  into N bands with N − 1 Linkwitz-Riley crossovers in series. A derived
  processor calls the protected `process_band()` for bands 0 to N − 1, in order,
  every block (asserted). The bands sum to the input through the all-passes of
  every crossover, so their sum before processing is the dry signal.
  `set_frequency()` clamps between the neighbouring crossovers and below
  Nyquist, and glides per sample over 5 ms; the prewarped gain is computed once
  per crossover and sample and shared by all its filters. The requested
  frequencies are kept, so `prepare()` at another sample rate clamps from them
  again; a non-finite one is ignored. `set_frequency()` blocks one crossover at
  its neighbours; `set_frequencies()` sets them all at once, independent of any
  order: crossing requests are sorted, so a crossover sweeping past another
  hands over continuously. `reset()` clears the crossovers. Tests:
  `BandSplitter*`.
- `dsp::utils::MultibandLimiterImpl`: one `LimiterImpl` per `BandSplitter` band,
  summed. Band count and order are constructor arguments; a derived class gives
  every band its threshold, attack and release through
  `get_band_parameter_float()`. `process_with_dry()` writes the sum of the bands
  before limiting, which matches the output below the thresholds (the limiters
  add no latency). `reset()` also releases the limiters. The crossover
  frequencies can come from `get_crossover_parameter_float()`, read at every
  block position and applied together with `set_frequencies()`, so State and
  Modulation drive them like the bands' parameters (non-finite, the default:
  `set_frequency()` decides). Tests: `MultibandLimiter.*`.
- `BaseProcessor::process_with_dry(buffer, modulation_offset, dry)`: also writes
  the dry signal, the input with the phase response of `process()` but without
  its effect, so that wet and dry can be blended. It is computed in the same
  pass, so it follows the same smoothing and modulation. `dry` is output-only;
  an empty `dry` means no dry signal. It is undelayed: a processor with
  `latency_samples()` leaves aligning it to whoever mixes it (a Graph delays a
  node's dry path by it). The default copies the input into `dry` and calls
  `process(buffer, modulation_offset)`: override it if `process()` changes the
  phase. `process_modulated_with_dry(buffer, dry[, offset])` and
  `process_modulated_with_dry(buffer, change_points, dry[, offset])` split both
  at the same change points. Tests: `BaseProcessor.*`.
- `TANH_LINK_COMPILE_COMMANDS` (default OFF; ON in the `desktop-*`, `macos-x64-*`,
  `android-*` and `wasm-release` presets): a top-level configure links
  `build/compile_commands.json` to its tree's database
  (`tanh_link_compile_commands`, tanh-tooling v0.3.0), so clangd follows the tree
  configured last, the wasm tree included.

### Changed

- **Breaking:** every declaration of every component lives in the inline namespace
  `thl::<ns>`: `thl::v0_5` in this release, `v<major>_<minor>` of the version by default (each
  opener reads `namespace thl::inline THL_ABI_NAMESPACE::core {`). Code keeps spelling `thl::...`,
  but every mangled name changes (`_ZN3thl4v0_5...`, `...@v0_5@thl@@`), so everything built
  against tanh-lib must be rebuilt. Two tanh-lib copies with different namespaces — a host's
  and a plugin's, a host's and anira's private core — no longer share code or state in one
  link; before, static archives on macOS, iOS and Windows gave both sides whichever copy the
  link order picked, and the header-only code merged by name on every platform. Migration:
  code that forward-declares a tanh-lib type itself (`namespace thl { class State; }`)
  declares a second, different type, which is ambiguous next to the header and does not link
  without it: include the tanh-lib header instead. A consumer built outside CMake needs the
  installed `tanh/core/AbiNamespace.h` on its include path (it sits next to the other core
  headers).
- **Breaking:** the Net component's Objective-C download delegate is
  `TanhDownloadDelegate_<ns>` (was `TanhDownloadDelegate`): Objective-C class names are
  global to the process, so two copies of Net in one process no longer clash in the runtime.
- The API reference is generated with the ABI namespace filtered out (a Doxygen
  `INPUT_FILTER`), so it documents `thl::core::Logger` as before.
- `Graph`: a node's wet/dry blend takes its dry signal from the node
  (`process_modulated_with_dry()`), phase-matched to its output, and delays it by
  the node's latency as before; a node that shifts the phase (a crossover, a
  multiband processor) no longer combs at a partial wet mix. A nested graph's dry
  signal is still its input. Tests:
  `Graph.ABlendOfANodeThatShiftsThePhaseUsesItsPhaseMatchedDrySignal`.
- `BaseProcessor` has the new virtual `process_with_dry()`, so its vtable
  changed: code that derives from it or calls it across a shared library must be
  recompiled.
- `BaseProcessor` holds its latency (two atomics), has a copy constructor and copy assignment
  and the virtual `reset()`: code that derives from it must be recompiled. `StereoFDN::reset()`
  and `DelayProcessor::reset()` now override it.
- `process_modulated()` no longer calls `process()` for an empty buffer.
- Shared tooling pinned to tanh-tooling v0.3.0 (`.clangd`, `cmake/tanh/`,
  `lint.yml`). `.clangd` now reads `build/compile_commands.json` instead of
  `build/desktop/Debug/`: reconfigure a development preset once to create the link.
  For the wasm tree, start clangd with `--query-driver=**/em++` (see
  `docs/sphinx/contributing.rst`, "Shared tooling").

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

[Unreleased]: https://github.com/tanh-lab/tanh-lib/compare/v0.5.0...HEAD
[0.5.0]: https://github.com/tanh-lab/tanh-lib/compare/v0.4.0...v0.5.0
[0.4.0]: https://github.com/tanh-lab/tanh-lib/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/tanh-lab/tanh-lib/compare/v0.2.0...v0.3.0
