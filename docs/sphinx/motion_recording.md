# Motion recording

`thl::modulation::MotionRecorder` records an XY gesture (x, y and the touch
gate) on the audio thread and loops it back, synced to the transport. A live
touch always overrides playback, and the output glides back to the running
loop on release.

- **Takes** are *free* (they end on release) or 1, 2, 4, 8 or 16 bars of the
  time signature at arm time (they end after exactly one loop).
- **Recording** runs at control rate into preallocated memory: 200 points per
  second, or `round(200 · 60 / bpm)` ticks per beat (24 to 512) while the
  transport plays. Points are stored raw; a raised-cosine blend closes the loop
  seam.
- **Smoothing** (`set_smoothing(amount)`, 0 to 1) is non-destructive: the lane
  stays raw and plays through a circular, zero-phase Gaussian over x and y
  (sigma up to a quarter beat, or 250 ms for Seconds lanes), computed on the
  message thread and published with the lane. The gate is not smoothed.
- **Playback** interpolates with uniform Catmull-Rom across the seam, clamped
  to [0, 1], evaluated every 32 samples with a linear ramp in between. Beats
  lanes follow the transport's tempo; Seconds lanes run on their own clock.
- **Finished takes** are published as `MotionLane` through `thl::RCU`, so the
  UI can draw them and presets can save them as JSON.

## Wiring

`XYController` ([XY controller](xy_controller.md)) owns one recorder per voice,
drives it every block and exposes the result as matrix sources; use
`controller.recorder(v)` for commands, `service()` and lanes. To drive a
recorder yourself, once per block on the audio thread:

```cpp
thl::modulation::MotionInput in;
in.m_x = touch_x;                  // [0, 1], one value per sample
in.m_y = touch_y;
in.m_active = touch_gate;          // 1 while touched
in.m_change_points = touch_changes;
in.m_num_samples = n;

recorder.process(clock.block_info(), in, n);  // n <= the prepared block size
// read recorder.out_x() / out_y() / out_gate() / out_live() / change_points()
```

Feed it the *pre-matrix* touch input, never a modulated value, so playback
cannot feed back as a fake touch.

| Output | Meaning |
|---|---|
| `out_x()`, `out_y()` | live touch while touched; otherwise the playing lane (with glides); otherwise the last value |
| `out_gate()` | 1 while touched or while the lane's gate is open |
| `out_live()` | 1 where the sample came from the live touch |
| `change_points()` | render ticks, touch and gate edges, glide starts |

## Commands

- `arm(len)`: the take starts on the first touch (or at once if a finger is
  down); until then the old lane keeps playing. `record(len)` starts at the next
  block without waiting. From its first sample the new take replaces the old
  lane: the old lane stops, and stays deleted if the take is dropped.
- `disarm()`: cancel arming and overdub. A running free take finishes and
  plays; a running bar take is dropped.
- With `MotionRecorderConfig::m_bar_aligned_takes`, every take is a Beats lane
  that starts on the bar line at or before its first touch (the time before the
  touch is recorded lifted, gate 0) and records lifts as gate 0. `BarsN` records
  exactly N bars. `Free` ends at the first of 1, 2, 4, 8 or 16 bars from its
  start reached with the finger up (16 at most); nothing is stretched.
  `disarm()` / `stop()` end a take at once, padded with gate 0 to N bars or the
  next free boundary; only a take that was never touched is dropped (the old
  lane then plays on).
- `overdub()`: while a lane plays, a touch writes (x, y, gate 1) into a copy of
  it at the playback positions under the finger (respecting playback length and
  reverse); untouched points and the length stay. Each release publishes the
  edit as a new take (one undo step per punch); the output glides back to the
  edited lane. With nothing playing, `overdub()` arms a take of the playback
  length first. `arm()`, `record()`, `disarm()` and `stop()` leave overdub.
- `undo()` / `redo()` (message thread): one level. Every publication (take,
  punch, load, clear) keeps the previous lane; `undo()` republishes it under a
  new take id and enables `redo()`; a new publication drops the redo lane.
  `clear_history()` forgets both (preset load).
- `set_playback_length(len)`: play every lane as `len`. `BarsN` stretches it onto
  exactly N bars of the current time signature from a bar line, faster or slower
  than recorded (a free take or a 2-bar take as one bar); `Free` plays the
  recorded length. The lane itself is unchanged.
- `stop()` / `play()`: playback off (gate 0, x and y hold) / on (with a glide).
  `stop()` also ends a running take like `disarm()`.
- `set_reverse(true)`: Beats lanes play the mirrored phase (they stay
  bar-locked), Seconds lanes turn around in place.
- `set_stopped_transport(Hold)`: freeze Beats lanes while the transport is
  stopped instead of free-running at the last tempo.

## Threading contract

- ctor, `prepare()`: message thread, audio stopped (allocates).
- `arm`, `record`, `overdub`, `disarm`, `play`, `stop`, `set_reverse`,
  `set_playback_length`: one UI thread,
  through a lock-free queue of `k_motion_command_capacity` (32) entries, applied
  at the next block start. They are `[[nodiscard]]` and return false when the
  queue is full; the command is then dropped.
- `set_stopped_transport()`, `smoothing()`: any thread.
- `process()`, the outputs and `snapshot()`: audio thread.
- `service()`, `load_lane()`, `clear()`, `undo()`, `redo()`, `can_undo()`,
  `can_redo()`, `clear_history()`, `set_smoothing()`: one message thread (RCU
  publication, allocates). Call `service()` from a timer at 10 Hz or more.
- `lane()` (raw), `played_lane()` (smoothed), `read_lane(f)`, `lane_version()`:
  message or UI thread; `lane_version()` bumps on every publication, including
  a smoothing change.

A UI reads the recorder's state from `XYFrame::m_voices[v].m_motion`
(a `MotionSnapshot`), not from the recorder.

### Take handoff

The RCU writer locks and allocates, so the audio thread never publishes. Each
recorder owns two take buffers. The audio thread writes one, finalises it in
place on finish and plays straight from it; `service()` copies it into a
`MotionLane` and publishes it; the next block switches to the published lane
(silently, or with a glide when it plays smoothed) and frees the buffer. An
overdub punch copies the playing lane into a free buffer (a bounded copy into
preallocated memory), plays and edits it, and hands it over the same way. If both buffers wait for
`service()`, arming waits and `MotionSnapshot::m_busy` is set. Take ids come
from one counter shared with `load_lane()` and `clear()`, and the newest id
always plays.

## Clock discontinuities

The transport's flags are the only jump detector (see
[Transport](transport.md), "Discontinuities"):

- no flag: the beat is continuous and the phase follows silently;
- `k_timeline_reset`: re-lock to the beat without a glide (also restarting the
  stopped free-run);
- `k_jumped` or `k_started`: re-lock and, if the lane phase moved by more than
  two points, glide from the last output. A jump onto the same phase (a DAW
  loop of whole lanes) changes nothing.

**Jump rule for a running take.** A discontinuity never aborts and never
shifts a running take: it keeps recording on its own clock, writes every index
once and ends after its length; only playback re-locks. Only `prepare()`
aborts a take (the previous lane is kept, `MotionSnapshot::m_aborted`).

## MotionLane and JSON

A `MotionLane` holds a take id, a timebase (Seconds or Beats), the loop length,
an anchor (Beats: index 0 sits at beat ≡ anchor mod length) and uniform x, y and
gate arrays.

```json
{"version":1,"take_id":7,"timebase":"beats","rate":96,"length":8.0,"anchor":0.0,
 "x":[0,1200,65535],"y":[32768,40000,51234],"gate":[[0,1],[2,0]]}
```

```cpp
state["motion"] = recorder.lane().to_json();                      // save
if (auto lane = thl::modulation::MotionLane::from_json(state["motion"])) {
    recorder.load_lane(std::move(*lane));                         // load: glides to it
}
```

x and y are stored as uint16, the gate as `[index, value]` edges.
`from_json()` never throws: it rejects a wrong version, missing or non-finite
fields and malformed arrays. `load_lane()` resamples a lane longer than the
capacity.

## Pitfalls

- **Block size.** `process()` takes at most the prepared block size (debug
  builds assert, release builds clamp). Split larger blocks with
  `TransportInfo::sub_block()`, or let `XYController` do it.
- **Memory.** Each recorder allocates two take buffers of
  `MotionRecorderConfig::m_max_points` (default 32768) points at 9 bytes each.
  A bar take that does not fit at 24 ticks per beat is refused
  (`MotionSnapshot::m_refused`).
- **Release tail.** A still tail before the release of at most one block is
  dropped from a free take: without event timestamps the release can arrive a
  block late.
- **Commands.** Check the return value: a full queue drops the command.
