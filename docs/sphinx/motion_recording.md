# Motion recording

`thl::modulation::MotionRecorder` records the gesture of one XY pad (x, y and
the touch gate) on the audio thread and loops it back, synced to the transport.
Draw a figure 8, and the same 8 plays back. A touch on the pad always overrides
playback, and the output glides back to the running loop on release.

- **Takes** are either *free* (as long as the gesture: they end on release) or
  1, 2, 4, 8 or 16 bars of the time signature at arm time (they end after
  exactly one loop).
- **Recording** runs at control rate into preallocated memory: 200 points per
  second, or `round(200 · 60 / bpm)` ticks per beat (24…512) while the
  transport plays.
- **Playback** interpolates with uniform Catmull-Rom across the loop seam,
  clamped to [0, 1], evaluated every 32 samples with a linear ramp in between.
- **Finished takes** are published as `MotionLane` values through `thl::RCU`
  so the UI can draw them and presets can save them as JSON.

## Per-block order

The recorder reads the pad's *pre-matrix* stream, so its own playback can never
feed back as a fake touch, and nothing depends on the matrix's source order.
Per block, on the audio thread:

```cpp
pad.process_block(n);                                  // 1. drain the touches (step A)
recorder.process(transport.block_info(), pad.primary(), n);  // 2. record / play / compose
// 3. read recorder.out_x() / out_y() / out_gate() / change_points() directly,
//    or let the matrix copy them through recorder.source(XYPadAxis::X / Y / Active)
matrix.process(n);
```

`XYController` ([XY controller](xy_controller.md)) does steps 1 and 2 in its
own driver step, in `pre_process_block()`, and writes the composed result into
its own output buffers, one pad and recorder per voice. The stand-alone sources
here are for using the recorder without such a controller. They are global-scope and not fully active (the active mask is the
output gate): route x/y with `ReplaceHold` and the gate with `Replace`.

`process()` produces per sample:

| Output | Meaning |
|---|---|
| `out_x()`, `out_y()` | live touch while touched; otherwise the playing lane (with glides); otherwise the last value |
| `out_gate()` | 1 while touched or while the lane's gate is open at the current phase |
| `out_live()` | 1 where the sample came from the live touch (layer reporting) |
| `change_points()` | render ticks (every 32 samples), touch and gate edges, glide starts |

## Threading

| Call | Thread | Mechanism |
|---|---|---|
| ctor, `prepare(sr, max_block, cfg)` | message, audio stopped | allocates take buffers, output, RCU reader slot |
| `arm(len)`, `record(len)`, `disarm()`, `play()`, `stop()`, `set_reverse()` | one UI thread | `LockFreeQueue<…, 32>` → audio, applied at the next block start |
| `set_stopped_transport()` | any | atomic |
| `process()` and the output accessors | audio | `TANH_NONBLOCKING_FUNCTION`: no allocation, lock or log |
| `service()`, `load_lane()`, `clear()` | one message thread | `RCU::replace` (allocates) |
| `lane()`, `read_lane(f)`, `lane_version()` | message / UI | RCU read; `lane_version()` bumps on every publication |
| `ui_snapshot()` | any | packed atomics written once per block |

Commands:

- `arm(len)`: the take starts on the first touch (or at once if a finger is
  already down); until then the old lane keeps playing.
- `record(len)`: start at the next block without waiting for a touch.
- `disarm()`: cancel arming. A running free take finishes (and plays); a
  running bar take is dropped (it has not covered its loop yet).
- `stop()` / `play()`: playback off (gate 0, x/y hold) / on (with a glide).
  `stop()` also ends a running take like `disarm()`.
- `set_reverse(true)`: Beats lanes play the mirrored phase (`length - phase`,
  so they stay bar-locked; the switch glides), Seconds lanes turn around in place.

### Take handoff

The tanh RCU writer locks and allocates, so the audio thread never publishes.
Each recorder owns two take buffers that move through
Free → Recording → Finished → Published → Free:

1. The audio thread writes only a Recording buffer.
2. On finish it finalises the buffer in place (seam blend, edge smoothing),
   stores Finished (release) and plays straight from it on the next sample.
3. `service()` (a ≥ 10 Hz timer on the message thread) sees Finished
   (acquire), copies the buffer into a `MotionLane`, publishes it with
   `RCU::replace` and stores Published.
4. The next block the audio thread sees the RCU lane with the same take id,
   switches to it without a glide (the data is bit-identical) and frees the
   buffer.

A new take always uses the other buffer. If both are waiting for `service()`
(the message thread stalled for a whole take), arming waits and
`ui_snapshot().m_busy` is set. Take ids come from one counter shared with
`load_lane()` and `clear()`, and the audio thread always plays the newest id:
a lane loaded after a take finished replaces it (`service()` then drops the
older take), a take finished after a load replaces that.

## Recording

- **Clock.** A take keeps its own beat clock, seeded at the first touch and
  advanced by the block's slope (the transport's `m_beats_per_sample` while it
  plays, the bpm while it is stopped). The clock absorbs tempo changes into the
  slope, also inside a block, so while the transport is continuous the take
  clock is the transport beat and the grid stays uniform in beats. What a jump
  does to a take: see "Jump rule" below.
- **Sampling.** A point is written at the first sample at or after each grid
  tick, from the live input at that sample.
- **Free take, transport stopped**: Seconds timebase, length = elapsed time. The
  take ends on release, on `disarm()`/`stop()` or when the capacity is full. A
  still tail before the release that is no longer than the current block is
  dropped: without event timestamps the release can arrive up to a block after
  the last move, and that would put a pause into the loop.
- **Free take, transport playing**: Beats timebase, length rounded to the
  nearest whole beat (minimum 1) and stretched at read time; anchor = the start
  beat rounded to 1/16.
- **Bar take**: `bars · sig_num · 4 / sig_denom` beats, anchor 0 (lanes line up
  with the bar grid). It starts at the first touch at any phase, writes index
  `(beat mod L) · tpb`, and finishes after exactly one loop, so every index is
  written once. After a release inside the take the gate is 0 and x/y hold. If
  the loop does not fit `m_max_points` at 24 ticks per beat the take is refused
  (`ui_snapshot().m_refused`).
- **Smoothing**: a centred 5-tap binomial `[1 4 6 4 1] / 16`, written two points
  behind (no phase lag). It removes touch jitter and the timing grain of evenly
  spread input events. The gate is not filtered.
- **Seam blend**: at finish, the step at the wrap beyond the expected velocity
  (the mean of three points on each side) is spread over the last
  `min(N / 4, 0.15 s)` points with a raised cosine, then the four edge points
  are smoothed across the seam. The looped curve is continuous in value and
  slope.

## Playback

- **Phase**: Beats lanes read `wrap(beat - anchor, length)` from the transport,
  or from a free-running clock at the last tempo while it is stopped
  (`StoppedTransport::FreeRun`, default; `Hold` freezes the output). Seconds
  lanes advance a sample clock and ignore the transport.
- **Tempo**: Beats lanes follow tempo changes and in-block ramps automatically.
- **Gate**: a step lookup per sample, so edges land on the exact sample.
- **Glides**: one 25 ms raised-cosine glide from the last output value to the
  running playback, used for a new lane id (take, preset load, `play()`), a
  transport jump and a touch release.
- **Touch override**: while the pad's gate is set the output is the live input,
  sample-accurate. Playback keeps running underneath, locked to the transport
  (DAW automation override), and on release the output glides back to it.
- **Render grid**: the ramp restarts at every loop wrap, so every loop renders
  on the same ticks.

### Clock discontinuities

The transport clock is the only jump detector: its `ContinuityTracker` decides
(see [Transport](transport.md), "Discontinuities"), and the recorder only reads
`TransportInfo`'s flags. A Beats lane takes its phase from the clock beat every
block; the flags decide what a change of that phase means:

- no flag: the beat is continuous (jitter and tempo changes, also inside a
  block, are already in the slope), so the phase follows silently. A source that
  never sets flags is treated the same way: there is no second detector;
- `k_timeline_reset` (first block after a clock `prepare()`/`reset()`,
  `XYController::reset()`): re-lock to the beat at once, without a glide. While
  stopped it also restarts the free-run from the transport's beat;
- `k_jumped` or `k_started` while playing: re-lock, restart the render ramp at
  offset 0 (never interpolating across the jump) and, if the phase moved by more
  than two lane points, glide from the last value actually output. A jump that
  lands on the same lane phase (a DAW loop of whole lanes, a Link realignment
  by a multiple of the lane length) changes nothing;
- while stopped the lane runs on the recorder's own free-run (or holds), so a
  seek while stopped takes effect at the next start (`k_started`).

**Jump rule for a running take.** A discontinuity (`k_jumped`, `k_timeline_reset`,
`k_started`, `k_stopped`) never aborts and never shifts a running take. The take
keeps recording on its own continuous clock, writes every index once and ends
after its length (a bar take after exactly one loop, a free take on release);
only playback re-locks to the transport. The finished lane keeps its anchor (bar
takes: 0, on the bar grid), so after a seek inside a bar take the part recorded
after the seek is on the grid of the take's own clock, not of the new song
position. Only `prepare()` aborts a take.

| Event | While playing | While recording |
|---|---|---|
| Host seek / scrub (`k_jumped`) | re-seek from the beat, glide | the take continues on its own clock; playback re-locks when it ends |
| Host loop wrap (`k_jumped`) | aligned with the lane: seamless; misaligned: re-seek and glide on each wrap | as above, the take is never aborted |
| Link join / phase realignment | glide unless the shift is a multiple of the lane length | as above |
| Tempo change (host, also inside a block; Link peer) | no jump: Beats lanes follow | the take integrates the new tempo and stays on the bar grid |
| Transport stop | `FreeRun`: keep moving at the last tempo; `Hold`: freeze | a bar take finishes on its own clock at the last tempo |
| Transport start (`k_started`) | re-lock to the new beat, glide if the phase moved | continue |
| Host reset (`XYController::reset()`, `k_timeline_reset`) | re-lock without a glide | continue |
| Time-signature change | lanes keep their length in beats | the take keeps the length chosen at arm time |
| `prepare()` | re-lock to the next block's beat without a glide | the take is aborted, the previous lane kept, `ui_snapshot().m_aborted` |

## MotionLane and JSON

```cpp
struct MotionLane {
    uint32_t m_take_id;              // 0 = never published; monotonic per recorder
    MotionTimebase m_timebase;       // Seconds | Beats
    double m_rate;                   // points per second | per beat when recorded
    double m_length;                 // loop length in seconds | beats
    double m_anchor;                 // Beats: index 0 sits at beat ≡ anchor (mod length)
    std::vector<float> m_x, m_y;     // [0, 1], uniform grid
    std::vector<uint8_t> m_gate;     // 0 / 1 per point
};
```

The read index of a phase is `phase · num_points / length`, so a lane may hold
more or fewer points than `length · rate` (a stretched free take, a legacy
ElasticFX lane at 24 ticks per beat).

```json
{"version":1,"take_id":7,"timebase":"beats","rate":96,"length":8.0,"anchor":0.0,
 "x":[uint16…],"y":[uint16…],"gate":[[0,1],[412,0],[600,1]]}
```

- x and y are quantised to uint16 (error ≤ 1/65535); the gate is stored as
  `[index, value]` edges.
- 4 bars at 120 BPM are 1600 points, about 20 KB.
- `MotionLane::from_json()` never throws. It rejects a wrong version, missing or
  non-finite fields, rate or length ≤ 0, x/y of different sizes or outside the
  uint16 range, and unsorted or out-of-range gate edges; a lane longer than the
  capacity is resampled down.

```cpp
// save (message thread)
state["motion"] = recorder.lane().to_json();
// load
if (auto lane = thl::modulation::MotionLane::from_json(state["motion"])) {
    recorder.load_lane(std::move(*lane));   // new take id; the audio thread glides to it
}
```

## Memory and cost

`MotionRecorderConfig::m_max_points` (default 32768) sets the capacity of each
of the two take buffers: 9 bytes per point (two floats and a gate byte), so
576 KiB per recorder, plus the published lane (its actual size) and one block
of output. 32768 points cover 16 bars of 4/4 at 20 BPM at about 170 points per
second. Playback of one recorder costs about 1.3 µs per 256-sample block on
Apple Silicon (`benchmark_modulation --benchmark_filter=motion`, release).
