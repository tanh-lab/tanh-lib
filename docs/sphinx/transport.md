# Transport and Ableton Link

Every `thl::dsp::transport::TransportClock` describes its current block as a
`TransportInfo`: a trivially copyable musical-time snapshot with discontinuity
flags. `InternalTransportClock` runs standalone, `HostTransportClock` follows a
plugin host's playhead and `LinkTransportClock` follows an Ableton Link session.

## TransportInfo

| Field | Meaning |
|---|---|
| `m_beat_position`, `m_beats_per_sample` | beat (quarter notes) at sample 0 and the slope; `beat_at(i)`, `beat_end()` |
| `m_bpm`, `m_sig_num` / `m_sig_denom` | tempo and time signature |
| `m_num_samples` | frames of the block |
| `m_flags` | validity (`k_has_tempo`, `k_has_beat_position`, `k_has_time_signature`), `k_is_playing`, discontinuities |
| `m_jump_delta_beats` | `beat(start) - expected`, with `k_jumped` |

Helpers: `phase(unit, offset)` (negative-safe: `-0.5` mod 4 is 3.5),
`division_in_block(Division)` (half-open: a boundary at the block end belongs
to the next block) and `sub_block(offset, length)` for processing a block in
chunks (the discontinuity flags stay on the first chunk).

| Flag | Raised when |
|---|---|
| `k_jumped` | seek, DAW loop wrap, Link phase realignment |
| `k_started` / `k_stopped` | play state changed against the previous block |
| `k_tempo_changed` | the tempo differs; the beat stays continuous |
| `k_timeline_reset` | first block after `prepare()` / `reset()`; with Link also the first realignment after Link was enabled or the first peer joined |

### Discontinuities

Every clock feeds its raw beats through a `ContinuityTracker`, which compares
the block's start beat with the previous block's end. Inside a window the start
snaps onto the expected beat and the slope is re-fitted to land on the source's
end beat; outside it the block is `k_jumped`. The window is
`[min(0, e) - tol, max(0, e) + tol]`:

- `tol` is the jitter tolerance (2 samples by default);
- `e = prev_frames · (bpm - prev_bpm) / (60 · sample_rate)`: a host reports one
  tempo per block, so a tempo step or ramp inside a block shows up at the next
  start as an offset of at most `e`, in the direction of the change.

`LinkTransportClock` widens the window by 50 ms of the tempo difference on both
sides (`set_tempo_window_samples()`, 0 for the other clocks): a Link peer dates
a tempo change at its own output time, which can lie before or after our block
start. The widening is off while a Link epoch change is pending (see below).

### Consumer contract

- Derive musical position from the absolute beat every block
  (`info.phase(loop_len)`, `floor(beat / step_len)`); never accumulate beats.
- The clock is the only jump detector. React to the flags and never compare
  beats against your own expectation: an unflagged block is continuous.
- `k_timeline_reset` is a jump that also clears edge detectors.
- A beat below zero (Link count-in) means "not started yet".

### Block size

A host may call with more samples than it announced. `ModulationMatrix::process()`
and `MotionRecorder::process()` take at most the prepared size (debug builds
assert, release builds clamp), so the engine splits the host block; only
`XYController` splits an oversized transport itself.

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

## HostTransportClock

Convert the host's playhead into a `TransportInfo` with validity flags and pass
it to `set_host_info()` before `begin_block()`:

```cpp
using TI = thl::dsp::transport::TransportInfo;
TI info;
if (auto bpm = p.getBpm()) { info.m_flags |= TI::k_has_tempo; info.m_bpm = *bpm; }
if (auto ppq = p.getPpqPosition()) { info.m_flags |= TI::k_has_beat_position; info.m_beat_position = *ppq; }
if (auto sig = p.getTimeSignature()) {
    info.m_flags |= TI::k_has_time_signature;
    info.m_sig_num = sig->numerator;
    info.m_sig_denom = sig->denominator;
}
if (p.getIsPlaying()) { info.m_flags |= TI::k_is_playing; }

clock.set_host_info(info);
clock.begin_block(static_cast<uint32_t>(buffer.getNumSamples()));
// ... consumers read clock.block_info() ...
clock.end_block();
```

Fields the host omits come from the fallbacks (`set_bpm()`,
`set_time_signature()`, `play()` / `stop()`). The host's play flag counts only
when it also sent tempo, beat or time signature. Without a host beat the clock
free-runs from the previous block's end; `set_position_beats()` moves the
free-run, and a seek made while the host supplies the beat waits until the beat
free-runs again.

## Ableton Link

`TANH_WITH_LINK=ON` (default OFF) adds the `tanh::Link` component: the Link
C++ SDK 4.1 (FetchContent, pinned to the `Link-4.1` commit) on macOS, Linux
and Windows, the LinkKit 4.1.2 release zip (pinned SHA256) on iOS; other
platforms are a configure error. Link is GPLv2+ or proprietary, LinkKit
proprietary only: a product built with the option must meet Link's terms. Use
Link in standalone apps; plugins follow the host's transport.

```cpp
thl::link::LinkSession session(120.0);                         // message thread
thl::dsp::transport::LinkTransportClock clock(session.audio_backend());
clock.prepare(sample_rate);
clock.set_output_latency_samples(device_output_latency);       // again on route changes
session.set_enabled(true);                                     // desktop; iOS: settings view

// audio callback
clock.begin_block(num_frames, host_time_us);  // or std::nullopt, see below
// ... clock.block_info() ...
clock.end_block();
```

- **Host time.** Pass the callback's host time in µs, without output latency,
  only when it is on Link's clock. Otherwise pass `std::nullopt`: the clock then
  maps its sample counter to Link time with a linear-regression filter.
- **Requests.** `set_bpm()`, `play()`, `stop()` and `set_position_beats()` are
  queued and applied at the block's output time; a local start is quantized to
  `set_quantum()` (default 4).
- **Running while stopped.** The Link timeline keeps moving while stopped
  (`m_beats_per_sample > 0`); gate musical events on `is_playing()`. Starts and
  stops dated ahead land in the block that contains them.
- **Joining.** After Link is enabled or the first peer joins (the timeline
  epoch changes), the first realignment within 1.5 s is `k_timeline_reset`;
  Link may publish the joined timeline after the epoch moved. A block captured
  between Link publishing the timeline and counting the peer reports that
  realignment as `k_jumped` only.
- **Tempo.** The clock never pushes the local tempo on its own; do not push a
  preset tempo while `num_peers() > 0`.

iOS apps also bundle `TANH_LINKKIT_RESOURCES_BUNDLE`, present
`session.settings_view_controller()` (enabling Link and start/stop sync are
user settings there), call `session.set_active(false)` when backgrounded without
audio, and set the Info.plist keys `NSLocalNetworkUsageDescription` and
`ABLLinkStartStopSyncSupported`. An installed static `tanh::Link` does not carry
LinkKit: the app links `LinkKit.xcframework` itself (configure prints a NOTICE).

## Threading contract

- `prepare()`: message thread, audio stopped.
- `set_host_info()`, `begin_block()`, `end_block()`, `block_info()` and the
  getters: audio thread, real-time safe.
- The setters (`set_bpm()`, `play()`, `set_output_latency_samples()`, ...):
  any thread, lock-free.
- `LinkSession`: message thread (may block); it outlives every clock built on it.

## Testing Link

`LinkTransportClock.*` (`test/dsp`, always built) checks the clock against a
scripted fake session, including sample-exact latency compensation. With
`TANH_WITH_LINK=ON`, `LinkSession.*` tests the real SDK backend, and on desktop
`LinkPeers.*` runs Ableton's
[TEST-PLAN](https://github.com/Ableton/link/blob/master/TEST-PLAN.md) cases
against an in-process peer. The peer tests need multicast on the local
interfaces, skip when the peers do not find each other within 10 s and carry
the CTest label `link-peers`; exclude them on CI with
`ctest --preset <preset> -LE link-peers`.
