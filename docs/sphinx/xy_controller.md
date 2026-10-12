# XY controller

`thl::modulation::XYController` puts touch input, a
[motion recorder](motion_recording.md) and a UI snapshot behind one object. It
exposes each dot ("voice") as three matrix sources: x, y and active. The UI
only sends touches and draws frames; voice selection, recording, playback and
the priority between them run in the controller.

## Construction and routing

```cpp
using namespace thl::modulation;

XYControllerConfig cfg;
cfg.m_id = "pad1";            // sources "pad1.x", "pad1.y", "pad1.active"
cfg.m_num_voices = 1;         // > 1: "pad1.<v>.x" ...
auto pad = std::make_unique<XYController>(matrix, cfg);   // message thread

pad->route(XYOutput::X, "cutoff");         // ReplaceHold
pad->route(XYOutput::Y, "resonance");
pad->route(XYOutput::Active, "wet");       // Replace: back to the base value on release
```

The constructor registers the sources; the matrix's `prepare()` prepares the
controller. `route(axis, target, options, voice)` creates a routing the
controller owns and returns its id. `XYRouteOptions` sets the combine mode,
priority, hold priority, depth, normalized replace range, enabled flag and
decimation; its defaults are those of `ModulationRouting`. The destructor
removes the owned routings and then the sources, waiting for the audio thread
to let go of them. The matrix must outlive the controller.

## Per block

Pass the block's transport to the matrix; every controller on it renders with
that transport:

```cpp
const auto& t = clock.block_info();
const auto scope = matrix.audio_read_scope();
matrix.process_with_scope(scope.data(), t.m_num_samples, &t);
```

The controller is a `BlockProcessor` of the matrix (`BlockProcessor.h`): the
matrix runs it once per block, before its sources' `pre_process_block()`. Per
voice it renders the touch input, runs the recorder and writes x, y, active,
the layer and the change points; the sources only copy those buffers, so
neither the matrix's source order nor the controller order matters.

- Without a transport (`matrix.process(n)`) the recorders free-run as on a
  stopped transport.
- Blocks are at most the prepared size; an engine splits larger host blocks
  (see [Transport](transport.md), "Block size").
- `reset()` (host reset) re-locks the recorders without a glide and closes
  latched gates.

## Layers

| Per sample | x / y | active | `XYLayer` |
|---|---|---|---|
| touched | the finger | 1 | `Touch` |
| motion playing, lane gate open | the lane | 1 | `Motion` |
| neither | hold the last value | 0 (1 while latched) | `None` |

The recorder already composes touch over playback and glides back after a
release, so there is exactly one glide. Once a voice has a value, its x and y
stay live, so a routing that is enabled later picks up the current position in
its first block. `set_latch(true)` keeps active at 1 after a release until latch
is turned off.

## Voices and touches

With one voice every finger drives it; `m_mono_priority` picks the finger
(`MonoPriority::Last` by default). With several voices, `touch(id, x, y)` maps a
new finger to the nearest enabled voice no other finger holds (or the nearest
enabled voice if all are held), and the finger keeps that voice until released.

- `set_voice_enabled(v, false)` excludes a voice from selection.
- `touch_voice(v, id, x, y)` skips selection (keyboard nudges, accessibility).
- `release(id)` on touch up *and* cancel; `release_all()` on unmount or focus
  loss. Coordinates are clamped to [0, 1] with y up; non-finite ones are rejected.
- When the queue is full, moves are coalesced and releases kept pending:
  `flush()` re-sends them, so no gate edge is lost.

### Timestamped touches

Untimed touches are spread over the block that drains them and held, so they
carry up to one block of jitter and a UI-rate staircase. Pass the event time
instead, and the block's callback time on the same clock:

```cpp
// UI thread (iOS: per coalesced UITouch, timestamp in seconds of uptime).
pad.touch(id, x, y, std::llround(touch.timestamp * 1e9));
// Audio thread, every block, before matrix.process().
pad.set_block_time(thl::core::uptime_ns());
```

- The block time holds for one `matrix.process()`. A host block split into
  chunks sets it per chunk: the callback time plus the chunk's offset at the
  sample rate.
- A touch plays at `time + m_input_delay_ms` (default one prepared block plus
  5 ms); one already late lands at offset 0, one due later waits for its block.
- x / y ramp linearly between two timed moves; down, up and a switch to another
  finger stay sharp. The ramp covers the whole gap only while the delay exceeds
  one block plus the gap between moves (plus arrival jitter); otherwise x / y
  hold until the next move arrives.
- `thl::core::uptime_ns()` (`tanh/core/Clock.h`) is the uptime clock on Apple platforms (the clock of
  `UITouch.timestamp`, `NSEvent.timestamp` and `AudioTimeStamp.mHostTime`) and
  `std::chrono::steady_clock` elsewhere. Any monotonic nanosecond clock works if
  both sides use it.

## Switching routings

To switch between sets of routings (one controller per target versus one
controller for several), keep all of them and toggle them with
`ModulationMatrix::set_routings_enabled()` (or `set_route_enabled()` for one).
A disabled routing writes nothing, Replace included, and drops its held value;
a batch reaches the audio thread complete or not at all, so no block sees
neither or both sets, and nothing is rebuilt. Depth 0 is not a substitute: a
Replace routing at depth 0 still writes `src * 0` and wins its target.

## UI snapshot

| Payload | Mechanism |
|---|---|
| `XYFrame`: per voice output, gate, layer, touched, path version and the recorder's `MotionSnapshot` (`m_motion`) | `thl::TripleBuffer<XYFrame>`, wait-free, one reader |
| live recording trail (`XYPathPoint`) | drop-oldest lock-free queue, `drain_trail(span)` |
| recorded path (`MotionLane`) | the recorder's RCU lane, `read_path(v, f)` when `m_path_version` changes |

Frames are built from the values of one block and published at most at
`m_frame_rate_hz` (240 Hz by default), at once on a discrete change.
`read_frame(out)` returns false and leaves `out` untouched when nothing is new.
A UI that does not drain the trail loses the oldest points
(`XYFrame::m_trail_dropped`); audio is unaffected. `set_ui_attached(false)`
stops frame and trail publication.

## Threading contract

- ctor, dtor, `route`, `unroute`, `set_route_enabled`, `set_route_depth`,
  `service`: message thread. Settings of the recorders go through `recorder(v)`
  or `for_each_recorder(f)`, with `MotionRecorder`'s threading.
- `touch`, `touch_voice`, `release`, `release_all`, `flush`, `read_frame`,
  `drain_trail`, `read_path`: one UI thread, lock-free.
- `set_latch`, `set_voice_enabled`, `set_ui_attached`: any thread.
- `recorder(v)`: as documented on `MotionRecorder`.
- `set_block_time`, `reset`, `out_*`: audio thread, real-time safe.

## Pitfalls

- Pass the transport to `matrix.process()` every block, or the recorders
  free-run instead of following it.
- Call `service()` from a message-thread timer (10 Hz or more), or finished
  takes are never published and arming stalls once both take buffers wait.
- A controller cannot be moved: the matrix holds pointers to its sources.
- Memory is dominated by the recorders' take buffers
  (`m_recorder.m_max_points`, two buffers of 11 bytes per point per voice);
  lower it for many voices.
