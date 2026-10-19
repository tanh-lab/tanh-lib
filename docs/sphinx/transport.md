# Transport

Every `thl::dsp::transport::TransportClock` describes its current block as a
`TransportInfo`: a trivially copyable musical-time snapshot with discontinuity
flags. `InternalTransportClock` runs standalone and `HostTransportClock` follows a
plugin host's playhead.

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
| `k_timeline_reset` | first block after `prepare()` / `reset()` |

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
assert, release builds clamp), so the engine splits the host block.

```cpp
for (size_t off = 0; off < num_samples; off += max_block) {
    const auto n = static_cast<uint32_t>(std::min(max_block, num_samples - off));
    clock.begin_block(n);
    matrix.process_with_scope(scope.data(), n, &clock.block_info());
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
free-run, and a seek made while the host supplies the beat is dropped.

## Threading contract

- `prepare()`: message thread, audio stopped.
- `set_host_info()`, `begin_block()`, `end_block()`, `block_info()` and the
  getters: audio thread, real-time safe.
- The setters (`set_bpm()`, `play()`, ...): any thread, lock-free.
