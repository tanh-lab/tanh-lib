# Transport, XY pad and Ableton Link

Three pieces turn musical time and touch input into modulation:

- `thl::dsp::transport::TransportInfo`: a per-block musical-time snapshot with
  discontinuity flags, produced by every `TransportClock` (host playhead,
  internal clock, Ableton Link).
- `thl::modulation::XYPad`: UI touches → sample-accurate `x` / `y` / `active`
  modulation sources.
- `thl::link::LinkSession` + `LinkTransportClock`: Ableton Link behind the CMake
  option `TANH_WITH_LINK`.

## TransportInfo

`TransportClock::block_info()` returns the current block as a trivially
copyable `TransportInfo` (so it can also be published to a UI thread through a
seqlock or triple buffer):

| Field | Meaning |
|---|---|
| `m_beat_position`, `m_beats_per_sample` | beat (quarter notes) at sample 0 and the slope; `beat_at(i)`, `beat_end()` |
| `m_bpm`, `m_sig_num` / `m_sig_denom`, `m_quantum` | tempo, time signature, phase unit (Link quantum, else beats per bar) |
| `m_bar_start_beats`, `m_loop_*_beats`, `m_host_time_ns` | valid with `k_has_bar_start`, `k_has_loop`, `k_has_host_time` |
| `m_num_samples` | frames of the block |
| `m_flags` | validity (`k_has_*`), state (`k_is_playing`, `k_is_looping`, `k_is_recording`), discontinuities |
| `m_jump_delta_beats` | `beat(start) - expected`, with `k_jumped` |

Helpers: `phase(unit, offset)` (negative-safe, `-0.5` mod 4 = 3.5),
`division_in_block(Division)` and the free `division_boundary_in(start, end, …)`
(half-open: a boundary at the block end belongs to the next block).

### Discontinuities

Every clock feeds its raw per-block beats through a `ContinuityTracker`. It
compares the block's start beat with the previous block's end (the expected
beat):

- inside the window the start snaps to the expected beat and the slope is
  re-fitted to land on the source's end beat. This absorbs host ppq jitter,
  host-time jitter and tempo changes inside a block without accumulating error;
  a consumer that integrates `m_beats_per_sample` lands on the source's beat;
- outside it the block gets `k_jumped` and `m_jump_delta_beats`.

The window is `[min(0, e) - tol, max(0, e) + tol]`:

- `tol` is the jitter tolerance: 2 samples worth of beats (0.5 ms for Link);
- `e = prev_frames · (bpm - prev_bpm) / (60 · sample_rate)` when the previous
  block moved, else 0. A host reports one tempo per block (JUCE, VST3: the tempo
  at the block start), and the clock predicts the block end with it. A tempo
  step or ramp inside the block (DAW tempo map, tempo automation, a Link peer)
  therefore shows up at the next block as a start difference of at most `e`, in
  the direction of the tempo change. Example: 512 samples at 48 kHz with 120 →
  80 BPM in the middle of the block is 0.0036 beats (85 samples at 120 BPM),
  far beyond the jitter tolerance but inside `e` = 0.0071 beats.

A seek smaller than `e` in the same direction as the tempo change is absorbed
too; any other seek, loop wrap or realignment is still `k_jumped`.

`LinkTransportClock` widens the window by `w = 50 ms · |bpm - prev_bpm| / 60`
beats on both sides (`ContinuityTracker::set_tempo_window_samples()`, 0 for the
other clocks). A Link peer dates a tempo change at its output time, which can lie
after our block start (the peer's output latency is larger than ours) or before
it (network and thread delays), and Link's timeline is a single line, so the new
tempo also applies before the change time. Measured with two in-process peers
(peer output latency 10 ms): changes dated up to 9 ms ahead and, on a heavily
loaded machine, 40 ms late, each flagged `k_jumped` without the widening. Not applied in a
block where the Link epoch changed (enable, first peer joined).

| Flag | Raised when |
|---|---|
| `k_jumped` | seek, DAW loop wrap, Link phase realignment |
| `k_started` / `k_stopped` | play state changed against the previous block |
| `k_tempo_changed` | bpm differs by more than 1e-6 (the beat stays continuous: not a jump, also for a change inside the previous block) |
| `k_timeline_reset` | first block after `prepare()`; with Link also when Link was enabled or the first peer joined *and* the beat moved |

### Consumer contract

Derive musical position *statelessly* from the absolute beat every block
(`phase = info.phase(loop_len)`, `step = floor(beat / step_len) mod n`); never
accumulate beats. Flags only reset internal state:

| Flag | Motion recorder | Step sequencer |
|---|---|---|
| `k_jumped` | playback: re-seek, glide from the last output if the lane phase moved (no click); a running take is *not* aborted, it continues on its own clock | recompute the step; retrigger only if the index changed |
| `k_started` | re-lock to the new beat, glide if the lane phase moved; a running take continues | retrigger the current step at the first sample with beat ≥ 0 |
| `k_stopped` | Beats lanes free-run at the last tempo (or hold, `StoppedTransport::Hold`); a bar take finishes on its own clock | gates off at offset 0 |
| `k_tempo_changed` | nothing (beat-stamped; a take integrates the new tempo) | nothing (steps are in beats) |
| `k_timeline_reset` | re-lock without a glide (also the stopped free-run); a running take continues | like `k_jumped` |
| beat < 0 (Link count-in) | phase wraps like any beat | not started |

The clock is the only jump detector. Consumers switch on the flags and never
compare beats against their own expectation: with a tempo-aware tracker an
unflagged block is continuous by contract, and a second detector with its own
threshold would disagree with the clock (it did: a tempo change inside a block
glided the motion recorder). A flagged jump that does not move a consumer's
phase (a DAW loop of whole lanes) needs no action. See
[Motion recording](motion_recording.md), "Clock discontinuities", for the
recorder and its jump rule for running takes.

### Block size

A host may call with more samples than the maximum it announced (JUCE does not
guarantee `numSamples <= maximumExpectedSamplesPerBlock` in every wrapper).
Who handles that:

- **The engine** splits the host block for everything that has per-sample
  buffers of the prepared length: `ModulationMatrix::process()` requires
  `num_samples <= samples_per_block` (debug builds assert, release builds clamp,
  so a violation cannot write past the buffers), and the DSP that reads the
  matrix's targets has the same limit. Run the clock, `set_transport()`,
  `matrix.process()` and the DSP once per chunk of at most the prepared size:

  ```cpp
  for (size_t off = 0; off < num_samples; off += max_block) {
      const auto n = static_cast<uint32_t>(std::min(max_block, num_samples - off));
      clock.begin_block(n);
      for (auto& pad : pads) { pad->set_transport(clock.block_info()); }
      matrix.process_with_scope(scope.data(), n);
      // DSP on samples [off, off + n) of the host buffer
      clock.end_block();
  }
  ```

- **`MotionRecorder::process()` and `XYController::process_block()`** (and the
  controller's driver step) accept a larger block on their own: they run it as
  consecutive chunks of at most the prepared size, with the chunk's transport
  from `TransportInfo::sub_block(offset, length)` (beat at the chunk start, the
  discontinuity flags on the first chunk only). The result is bit-identical to
  the same audio in prepared-size blocks: takes, playback phase, glides, jumps
  and UI frames advance by the whole block, so their clock stays with the
  transport. Their output buffers then hold the last chunk (`num_samples()` is
  its length). Tests: `OversizedBlocks.*`.

### Clocks

| Clock | Use | Notes |
|---|---|---|
| `InternalTransportClock` | standalone without Link | tempo changes keep the beat continuous |
| `HostTransportClock` | plugins (AU, AUv3, VST3, CLAP) | `set_host_info()` per block; fallbacks for omitted fields |
| `LinkTransportClock` | standalone with Link | over a `LinkBackend`; see below |

`HostTransportClock` takes the host's view each block, before `begin_block()`.
Tempo comes from the host when it sends one (else `set_bpm()`); the play state
when the host sent any musical field (else `play()` / `stop()`); the beat when
the host sent one, else it free-runs from the previous block's end without
drift (`set_position_beats()` moves the free-run). A JUCE adapter in the plugin
is a few lines:

```cpp
thl::dsp::transport::TransportInfo to_transport_info(const juce::AudioPlayHead::PositionInfo& p) {
    using TI = thl::dsp::transport::TransportInfo;
    TI info;
    if (auto bpm = p.getBpm()) { info.m_flags |= TI::k_has_tempo; info.m_bpm = *bpm; }
    if (auto ppq = p.getPpqPosition()) { info.m_flags |= TI::k_has_beat_position; info.m_beat_position = *ppq; }
    if (auto sig = p.getTimeSignature()) {
        info.m_flags |= TI::k_has_time_signature;
        info.m_sig_num = sig->numerator;
        info.m_sig_denom = sig->denominator;
    }
    if (auto bar = p.getPpqPositionOfLastBarStart()) { info.m_flags |= TI::k_has_bar_start; info.m_bar_start_beats = *bar; }
    if (auto loop = p.getLoopPoints()) {
        info.m_flags |= TI::k_has_loop;
        info.m_loop_start_beats = loop->ppqStart;
        info.m_loop_end_beats = loop->ppqEnd;
    }
    if (auto ns = p.getHostTimeNs()) { info.m_flags |= TI::k_has_host_time; info.m_host_time_ns = static_cast<int64_t>(*ns); }
    if (p.getIsPlaying()) { info.m_flags |= TI::k_is_playing; }
    if (p.getIsLooping()) { info.m_flags |= TI::k_is_looping; }
    if (p.getIsRecording()) { info.m_flags |= TI::k_is_recording; }
    return info;
}

// processBlock():
clock.set_host_info(to_transport_info(*getPlayHead()->getPosition()));
clock.begin_block(static_cast<uint32_t>(buffer.getNumSamples()));
// ... consumers read clock.block_info() ...
clock.end_block();
```

## XY pad

`XYPad` turns `touch(id, x, y)` / `release(id)` from one UI thread into three
matrix sources. x and y travel through one lock-free queue together, so a
diagonal drag changes both axes at the same in-block offsets. Coordinates are
normalised by the caller with y pointing up; they are clamped to `[0, 1]` and
NaN is rejected.

```cpp
thl::modulation::XYPad pad({.m_max_touches = 1});     // global scope, one finger
pad.add_to(matrix, "xy.pad1");                         // "xy.pad1.x" / ".y" / ".active"

thl::modulation::ModulationRouting r("xy.pad1.x", "slot_a");
r.m_combine_mode = thl::modulation::CombineMode::ReplaceHold;  // keep the last touch
r.m_replace_priority = 10;
matrix.add_routing(r);

// UI thread (lock-free, allocation-free)
pad.touch(finger_index, x, 1.0f - y_screen);
pad.release(finger_index);   // on touch up *and* cancel
pad.release_all();           // on unmount / focus loss
// pad.remove_from(matrix) before the pad or the matrix is destroyed
```

- **Slots**: a new id takes the first free slot (up to `m_max_touches`, cap
  16); surplus fingers are ignored until released.
- **Global scope with several touches** reduces to one output on the UI side:
  `MonoPriority::Last` (newest finger drives; releasing it falls back to the
  newest remaining finger's latest position) or `First`.
- **Voice scope**: slot = voice; simultaneous touches land at offset 0.
- **Buffers** hold value and gate across blocks. A down/move writes x/y from
  its offset with `active = 1`; an up keeps x/y and drops `active` to 0. Route
  x/y with `ReplaceHold`, `active` with `Replace`.
- **Queue full** (audio thread stalled): moves coalesce per stream, a release
  stays pending and is sent first, a new finger that cannot be queued is
  rejected (`touch()` returns false). `flush()` retries; no gate edge is lost.

### Driving the pad from an owner

An XY controller that records motion or sequences steps needs the pad's state
*before* the matrix routes it (`XYController` does this, see
[XY controller](xy_controller.md)). Call `process_block(n)` on the audio thread (for
example from the controller's own source's `pre_process_block()`, with `n` from
`TransportInfo::m_num_samples`), then read `stream(i)` / `primary()`
(`XYPadStream`: x, y, active and the change points for `n` samples). If the
pad's own outputs are also in the matrix, they reuse that render instead of
draining twice. Calling `process_block(n)` before `matrix.process(n)` is also
how hosts with variable block sizes get events spread over the real block;
otherwise the outputs spread over the prepared maximum block size.

## Ableton Link

`TANH_WITH_LINK=ON` (default OFF) adds the `tanh::Link` component:

| Platform | SDK | Fetched as |
|---|---|---|
| macOS, Linux, Windows | Ableton Link C++ SDK 4.1 (header-only, asio-standalone submodule) | `FetchContent` git tag `Link-4.1`, recursive submodules |
| iOS | LinkKit 4.1.2 (`LinkKit.xcframework`, C API) | official release zip, pinned `URL_HASH SHA256` |
| Android, Web | — | configure error (unverified) |

Licence: Link is GPLv2+ or proprietary, LinkKit proprietary only. Without the
option no Link code is fetched or linked; a product built with it must meet
Link's terms (GPL, or Ableton's proprietary Link licence). Use Link in a standalone app only;
plugins follow the host's transport (`HostTransportClock`).

```cpp
thl::link::LinkSession session(120.0);                         // message thread
thl::dsp::transport::LinkTransportClock clock(session.audio_backend());
clock.prepare(sample_rate);
clock.set_output_latency_samples(device_output_latency);       // again on route changes
session.set_enabled(true);                                     // desktop; iOS: settings view

// audio callback: host time of the callback in µs on the Link clock, no latency
clock.begin_block(num_frames, host_time_ns / 1000);
// ... clock.block_info() ...
clock.end_block();
```

- The clock adds the output latency (`samples * 1e6 / sample_rate` µs) to the
  callback time, captures the session, applies queued `set_bpm()` / `play()` /
  `stop()` / `set_position_beats()` requests at that time (a local start
  requests beat 0 at the start time, quantized to `set_quantum()`), commits only
  when something changed, and evaluates the beat at the block's start and end.
- Without a host time a linear-regression filter maps the sample counter to the
  Link clock (Ableton's `HostTimeFilter`, fixed storage).
- The Link timeline keeps running while stopped (`m_beats_per_sample > 0`);
  gate musical events on `is_playing()`. A peer's start lands as `k_started` in
  the block that contains the start time; beats before it are negative. A stop
  dated ahead (a peer stopping at its output time) lands as `k_stopped` in the
  block that contains it.
- The clock never pushes the local tempo on its own (TEMPO-5). Do not push a
  preset tempo while `num_peers() > 0` (TEMPO-2/3).
- A disabled session is still a local timeline, so enabling or disabling Link
  never swaps clocks or moves the beat (BEATTIME-1).

iOS apps also bundle `TANH_LINKKIT_RESOURCES_BUNDLE` (`LinkKitResources.bundle`),
present `session.settings_view_controller()` (an `ABLLinkSettingsViewController*`;
LinkKit requires it, and enabling Link and start/stop sync are user settings
there), call `session.set_active(false)` when backgrounded without audio, and set
the Info.plist keys `NSLocalNetworkUsageDescription` and
`ABLLinkStartStopSyncSupported`.

### Testing Link

Ableton's [TEST-PLAN](https://github.com/Ableton/link/blob/master/TEST-PLAN.md)
is covered at three levels:

| Where | Built when | Runs | Covers |
|---|---|---|---|
| `test/dsp` `LinkTransportClock.*` | always | every `ctest` | clock logic against a scripted fake session, no network; `LatencyCompensatedClicksAreSampleExact` proves the latency math sample-exact (AUDIOENGINE-1 math) |
| `test/link` `LinkSession.*` | `TANH_WITH_LINK=ON` | every `ctest` | the real SDK backend: disabled session, BEATTIME-1, local start/tempo, RTSan, a two-peer smoke test |
| `test/link` `LinkPeers.*`, `BlockSizes/LinkPeersLatency.*` | `TANH_WITH_LINK=ON`, desktop | label `link-peers` | TEST-PLAN cases against a LinkHut-like peer on the raw Link SDK in the same process |

`LinkPeers` drives `LinkSession` + `LinkTransportClock` (the app's code path) with
simulated audio blocks (48 kHz, fixed block size, exact host times rendered once
the Link clock has passed them) and asserts against the peer's session state:

| Test | TEST-PLAN |
|---|---|
| `Tempo1_OurTempoChangeReachesPeer`, `Tempo1_PeerTempoChangeReachesUsWithoutJump` | TEMPO-1 (both ways, while playing: `k_tempo_changed`, no `k_jumped`) |
| `Tempo2_JoiningKeepsTheSessionTempo` | TEMPO-2 |
| `Tempo4_ExtremeTempi` | TEMPO-4 (20 and 999 BPM) |
| `Tempo5_EnableDisableKeepsOurTempo` | TEMPO-5 (and BEATTIME-1) |
| `PhaseAlignedAfterJoin` | phase (quantum 4) within 0.25 ms of the peer |
| `Beattime2_PeerJoiningDoesNotMoveOurBeat` | BEATTIME-2 |
| `StartStop1_PeerStartsAndStopsUs`, `StartStop2_WeStartAndStopThePeer` | STARTSTOPSTATE-1/2 |
| `BlockSizes/LinkPeersLatency.*` (64, 512, 2048 frames, 10 ms output latency) | AUDIOENGINE-1 stand-in: clicks within 3 ms of the peer's beats |

Measured on an Apple Silicon Mac: phase error 4–20 µs, click error ≤ 16 µs.
Each test prints its measurements as `[ measured ]` lines.

These tests need multicast on the loopback/local interfaces and are known to be
flaky on CI runners (as for SuperCollider's LinkClock): they skip when the peers
do not discover each other within 10 s or a foreign Link peer is on the network,
poll with generous timeouts, and all Link tests share the CTest
`RESOURCE_LOCK link-network` so `ctest -j` never runs two at once. Exclude them in
CI with:

```sh
ctest --preset <preset> -LE link-peers
```

Run only them with `ctest -L link-peers` (about 45 s).

The manual TEST-PLAN run (against LinkHut on macOS and iOS, including
AUDIOENGINE-1 with a loopback recording) lives in the product repository: run it
before a release.
