# Motion recording

`thl::modulation::MotionRecorder` records an XY gesture (x, y and the touch
gate) on the audio thread and loops it back, synced to the transport. A live
touch always overrides playback, and the output glides back to the running
loop on release.

- **Takes** are Beats loops of whole bars. *Free* records one gesture, from the
  touch to the release, and stretches it onto the nearest of 1, 2, 4, 8 or 16
  bars. *BarsN* is a looper over N bars that records until `disarm()`: the
  finger overwrites where it is down, the loop plays on where it is up.
- **Recording** runs at control rate into preallocated memory:
  `round(200 · 60 / bpm)` points per beat (24 to 512), on the transport's beat
  while it plays, else on the recorder's own free-running beat. Points are stored
  raw, including the step where the newest point meets the oldest (the seam).
- **Smoothing** (`set_smoothing(amount)`, 0 to 1) is non-destructive: the lane
  stays raw and a played copy is computed on the message thread and published
  with it. With `LoopEnd::Smooth` (default) the copy spreads the seam step
  (beyond the expected velocity) over a raised-cosine window from 0.3 beat
  (150 ms for Seconds lanes) at amount 0 to the whole loop at 1, so a take that
  did not quite end where it started still loops without a jump. Then x and y
  run through a circular, zero-phase Gaussian with sigma = amount² × half a
  beat (250 ms for Seconds lanes; 0.2 gives 0.02 beat, 10 ms at 120 bpm),
  capped at a sixth of the loop. The gate is not smoothed. The same curve comes from
  `make_played_curve(lane, amount, loop_end)` for any lane.
- **Loop end** (`set_loop_end(LoopEnd::Jump)`): keep the seam step as a hard
  edge (a drawn line that jumps back to its start). The Gaussian then treats
  the take as an open sequence (mirrored at both ends) and does not smooth
  across the edge. Like a smoothing change it republishes and glides.
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

## Components

`MotionRecorder` is a thin facade over four parts, each tested on its own:

| Part | Does |
|---|---|
| `detail::MotionTakeRecorder` | records a take into one working buffer (Free, BarsN) |
| `detail::MotionLaneStore` | hands takes to the message thread, publishes lanes through RCU, undo |
| `detail::MotionLanePlayer` | plays a lane on the clock: phase, playback length, reverse, jumps, render ramp, the transform |
| `detail::MotionCompositor` | the output: touch over playback over hold, glides, change points |

`detail::MotionClock` gives every block its beats (the transport, or the free run).

## Commands

- `arm(len)`: the take starts on the first touch (or at once if a finger is
  down); until then, and while it records, the lane that played plays on.
  `record(len)` starts at the next block without waiting (a free take still
  starts its points at the touch).
- `disarm()`: cancel arming; a running take ends there and plays.
- A take that keeps nothing (never touched, or a free take too short) leaves the
  lane that played: it plays on.
- **Free**: from the first touch to the release, 16 bars of the tempo at its
  start at most. The still ends (the finger down but not yet or no longer
  moving) are dropped and the take is stretched onto the nearest (by ratio) of
  1, 2, 4, 8 or 16 bars, point 0 on the bar line nearest its first point: it
  loops as one continuous movement, without a pause.
- **BarsN**: a loop of N bars from the loop line (the multiple of N bars at or
  before the start, where `set_playback_length(BarsN)` plays it). It starts empty
  (the lane that played stops when it starts; it is replaced, or deleted if the
  take keeps nothing) and records until `disarm()` or `stop()`: where the finger
  is down it overwrites, where it is up the loop keeps what it holds. Every
  release hands the loop over, gliding through the time the finger never drew
  (from where it lifted to where the drawing goes on, around the loop line, gate
  open), so the loop never stands still and between touches it plays smoothed and
  transformed like any lane; the whole take is one undo step. Its seam is where the newest point
  meets the oldest.
- `undo()` / `redo()` (message thread): one level. Every publication (take,
  load, clear) keeps the previous lane, except the later revisions of one take;
  `undo()` republishes it under a new take id and enables `redo()`; a new
  publication drops the redo lane. `clear_history()` forgets both (preset load).
- `set_playback_length(len)`: play every lane as `len`. `BarsN` stretches it onto
  exactly N bars of the current time signature from a bar line, faster or slower
  than recorded; `Free` plays the recorded length. The lane itself is unchanged.
- `stop()` / `play()`: playback off (gate 0, x and y hold) / on (with a glide).
  `stop()` also ends a running take like `disarm()`.
- `set_reverse(true)`: Beats lanes play the mirrored phase (they stay
  bar-locked), Seconds lanes turn around in place.
- `set_stopped_transport(Hold)`: freeze Beats lanes while the transport is
  stopped instead of free-running at the last tempo.

## Threading contract

- ctor, `prepare()`: message thread, audio stopped (allocates).
- `arm`, `record`, `disarm`, `play`, `stop`, `set_reverse`,
  `set_playback_length`: one UI thread,
  through a lock-free queue of `k_motion_command_capacity` (32) entries, applied
  at the next block start. They are `[[nodiscard]]` and return false when the
  queue is full; the command is then dropped.
- `set_stopped_transport()`, `smoothing()`, `loop_end()`: any thread.
- `process()`, the outputs and `snapshot()`: audio thread.
- `service()`, `load_lane()`, `clear()`, `undo()`, `redo()`, `can_undo()`,
  `can_redo()`, `clear_history()`, `set_smoothing()`, `set_loop_end()`: one
  message thread (RCU publication, allocates). Call `service()` from a timer at 10 Hz or more.
- `lane()` (raw), `played_lane()` (what plays: seam closed and smoothed),
  `read_lane(f)` (raw), `lane_version()`: message or UI thread;
  `lane_version()` bumps on every publication, including a smoothing or loop
  end change.

A UI reads the recorder's state from `XYFrame::m_voices[v].m_motion`
(a `MotionSnapshot`), not from the recorder.

### Take handoff

The RCU writer locks and allocates, so the audio thread never publishes. The
take records into its own working buffer; a release (BarsN) or the end of a take
copies the loop into one of two hand-over slots, closes its seam in place for
`LoopEnd::Smooth` (keeping the raw values of those points) and plays it from that
sample. `service()` copies a slot into a `MotionLane`, restores the raw values and
publishes it; the next block that reads the new lane frees the slot (silently, or
with a glide when it plays smoothed). If both slots wait for `service()`, a
finished take waits too (`MotionSnapshot::m_busy`) and plays once a slot is free;
a BarsN revision is skipped (the next one carries the loop). Take ids come from
one counter shared with `load_lane()` and `clear()`, and the newest id always
plays.

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
shifts a running take: it keeps recording on its own clock (the transport's
beats per sample, or the free run), so a BarsN take writes every point of its
loop on that clock; only playback re-locks. Only `prepare()` aborts a take (the
lane that played is kept, `MotionSnapshot::m_aborted`).

## Transforms

`set_transform(const MotionTransform*)` plugs the application's path into
playback (shapes, scaling, rotation: the recorder has none of its own). A
transform is stateless; its setting is one 64-bit word the application packs and
stores with `set_transform_settings()` (any thread), so every block and every
drawing reads a whole, consistent setting. `MotionTransform::info(settings)` says
what it does:

- inactive: the lane plays as it is;
- `m_uses_lane`: `place()` moves the lane's point at each position in the loop;
- `m_plays_alone`: without a lane it loops over the playback length (one bar
  while Free), from the bar line;
- `m_opens_gate`: the gate stays open while it sounds;
- `m_curve`: a new value (another path) re-seeks and glides like a new lane;
  amounts are read once per block and follow at the render interval, so host
  automation is smooth.

A touch overrides it as it does the lane. `played_path(n)` (and
`render_motion_path()`) samples what plays over one loop for drawing;
`transform_version()` changes with the setting.

## MotionLane and JSON

A `MotionLane` holds a take id, a timebase (Seconds or Beats), the loop length,
an anchor (Beats: index 0 sits at beat ≡ anchor mod length), the seam (index of
the first recorded point, not 0 for a bar take started mid-loop; written only
when not 0) and uniform x, y and gate arrays.

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
- **Memory.** Each recorder allocates a take buffer and two hand-over slots of
  `MotionRecorderConfig::m_max_points` (default 32768) points at 9 bytes each
  (x, y and gate), plus a raw seam tail of a quarter of the points per slot.
  A take that does not fit at 24 points per beat is refused
  (`MotionSnapshot::m_refused`).
- **Still ends.** A free take drops the time the finger is down but not yet or
  no longer moving: without event timestamps the release can arrive a block late.
- **Commands.** Check the return value: a full queue drops the command.
