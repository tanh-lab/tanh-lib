# XY controller

`thl::modulation::XYController` puts an XY pad, a motion recorder and a UI
snapshot behind one object. It exposes each dot ("voice") as three matrix
sources: x, y and active. An effect with several slots uses one of two layouts:

- **PerEffect**: one controller per slot, each with one voice.
- **Single**: one controller with one voice per slot. A touch grabs the
  nearest active dot, as in the legacy ElasticFX app.

`XYModeRouter` switches between the two without a schedule rebuild.

The UI only sends touches and draws frames. Voice selection, recording,
playback and the priority between them all run in the controller.

## Construction and registration

```cpp
thl::modulation::XYControllerConfig cfg;
cfg.m_id = "pad1";            // sources "pad1.x", "pad1.y", "pad1.active"
cfg.m_num_voices = 1;         // > 1: "pad1.<v>.x" …
auto pad = std::make_unique<thl::modulation::XYController>(matrix, cfg);  // message thread

pad->route(XYPadAxis::X, "s1.a");             // ReplaceHold, priority 10, hold priority 0
pad->route(XYPadAxis::Y, "s1.b");
pad->route(XYPadAxis::Active, "s1.wet");      // Replace: falls back to the base value on release
```

The constructor registers the sources with the matrix. `route()` creates a
routing and the controller records that it owns it. Options are set through
`XYRouteOptions`: combine mode, priority, hold priority, depth, normalized
replace range, enabled flag and decimation. `route_voices(axis, targets)`
routes voice *v* to `targets[v]`.

The destructor removes the owned routings and then the sources. It runs on the
message thread and waits for the audio thread to let go of them. The matrix must
outlive the controller. A controller cannot be moved, because the matrix holds
pointers to its outputs.

## Per block

`pre_process_block()` is not given the block length, so the controller gets it
from the block's `TransportInfo` (`m_num_samples`). Give that transport to
every controller before `matrix.process()`:

```cpp
const TransportInfo t = clock.block_info();     // m_num_samples = this block
for (auto& pad : pads) { pad->set_transport(t); }
const auto scope = matrix.audio_read_scope();
matrix.process_with_scope(scope.data(), t.m_num_samples);
```

`set_transport()` copies the 88-byte value. A copy was chosen over a
`TransportClock&` for three reasons:

- The engine already builds one `TransportInfo` per block.
- The controller then works with any clock (host, internal, Link) and in
  tests without one.
- There is no pointer whose lifetime must be managed.

Without a fresh `set_transport()`, debug builds assert. Release builds continue
the previous transport and count `stale_transport_blocks()`.

When the matrix calls `pre_process_block()` on the first of a controller's
outputs, the controller runs its **driver step** once for that block. For each
voice the step does:

```cpp
pad.process_block(n);                            // touch layer (pre-matrix)
recorder.process(t, pad.primary(), n);           // motion layer; touch > playback
// [extension point] a sequencer layer would advance here
// write x / y / active / layer / change points for the whole block
```

The output sources only copy those buffers in `process()`. It does not matter
which output the matrix reaches first, or in which order it registers
controllers. Controllers share no state.

Without a matrix, `process_block(t)` runs the driver directly. If the
controller is also in a matrix, its outputs reuse that render in the same block.
A transport longer than the prepared block size runs as consecutive chunks of
the prepared size, with the same result as prepared-size blocks; the outputs
then hold the last chunk. The matrix itself takes no block above the prepared
size, so an engine splits host blocks (see [Transport](transport.md), "Block
size").
`reset()` (audio thread, host reset) makes the next block a timeline reset, so
the recorders re-lock without a glide. It also closes latched gates.

## Layers

| Per sample | x / y | active | `XYLayer` |
|---|---|---|---|
| touched | the finger | 1 | `Touch` |
| motion playing, lane gate open | the lane (Catmull-Rom) | 1 | `Motion` |
| neither | hold the last value | 0 (1 while latched) | `None` |

`MotionRecorder` already composes touch over playback. After a release it
glides back to the running loop at the current phase (25 ms raised cosine,
`MotionRecorderConfig::m_glide_ms`). The controller uses the recorder's output
as is, so there is exactly one glide and no second smoothing stage. A
touch-down jumps on purpose. The tests check that the controller output is
bit-identical to a standalone pad and recorder.

x / y masks become live from the first sample at which the voice has a value,
whether touched or played. After that they stay live, so x / y routings always
see the dot's current position. As a result, a routing that is re-enabled by a
mode switch picks the dot up in its first block. ReplaceHold behaves exactly
like Replace here. The active mask is the gate.

`set_latch(true)` gives Kaoss-style hold: after a touch is released, active
stays 1 until latch is turned off. Turning it off records a change point at
offset 0.

## Voices and touches

With one voice, every finger drives that voice. Up to `m_max_touches`
fingers are tracked, and the newest wins (`MonoPriority::Last`).

With several voices, `touch(id, x, y)` maps a new finger to the **nearest
enabled voice that no other finger holds**. If every enabled voice is held,
the finger goes to the nearest enabled voice. The finger keeps that voice
until it is released, even if it is dragged across other dots. Only that voice
moves.

- Selection runs on the UI thread against each dot's position at the end of
  the last block (one relaxed 64-bit atomic per voice).
- `set_voice_enabled(v, false)` excludes a voice, for example an inactive
  slot.
- `touch_voice(v, id, x, y)` skips selection, for keyboard nudges and
  accessibility.
- `release(id)`, `release_all()` and `flush()` work as on `XYPad`.

## Single / PerEffect

A `depth` of 0 does not silence a Replace routing: it still writes `src * 0`
and wins the target. Mode switching therefore uses
`ModulationMatrix::set_routing_enabled()`:

- A disabled routing writes nothing to its target: no Additive term, no
  Replace value and no hold.
- Disabling a routing clears its hold, so a routing that is enabled again does
  not bring back a stale value.
- Both the disable and the enable edge flag a change point at offset 0.
- The flag is saved in the routing JSON as `"enabled": false`.

`set_routings_enabled(batch)` applies several flags so that every audio block
sees all of them or none. The audio thread reads the flags once per block
through a seqlock. It never waits: if a block overlaps a write, it keeps the
previous flags for one more block.

```cpp
XYModeRouter router(matrix, XYPadMode::PerEffect);
for (int n = 0; n < 8; ++n) {
    for (auto [param, axis] : {std::pair{"a", XYPadAxis::X}, std::pair{"b", XYPadAxis::Y}}) {
        router.add_target(slot(n, param), axis,
                          *per_effect[n], 0,       // enabled in PerEffect, priority 10
                          *single, n);             // enabled in Single, priority 20
    }
}
router.set_mode(XYPadMode::Single);                // message thread, no rebuild
```

`set_mode()` is a single batch, so no block sees neither layout or both. When
the PerEffect dot and the Single dot of a slot are in the same place, a mode
switch causes no step at all. When they are in different places, the slot
moves from one dot to the other in one block. Smooth that in the slot's
parameter smoother if it is audible. The routings belong to the controllers;
the router only keeps their ids.

## UI snapshot

| Payload | Size | Mechanism |
|---|---|---|
| `XYFrame`: per voice output, gate, layer, touched, motion state/phase/progress, take id, path version | 672 B, fixed | `thl::TripleBuffer<XYFrame>`: wait-free on both sides, one reader |
| live recording trail (`XYPathPoint`) | one point per voice every 32 samples while recording | `LockFreeQueue<XYPathPoint, 2048>::push_overwrite`, drop-oldest |
| recorded path (`MotionLane`) | up to the recorder capacity | the recorder's RCU lane; reread when `m_path_version` changes |

- **Frames.** The audio thread builds each frame from the values it computed
  in that block. The recorder's `ui_snapshot()` fields can be a block apart
  when read from the UI, but on the audio thread, right after `process()`,
  they are exact. Frames are published at most every
  `sample_rate / m_frame_rate_hz` samples (240 Hz by default). A discrete
  change (touch, gate, layer, motion state, take, path version or latch) is
  published at once.
- **Reading frames.** `read_frame(out)` returns false and leaves `out`
  untouched when nothing new has been published. `set_ui_attached(false)`
  stops frame and trail publication.
- **Trail.** `drain_trail(span)` pops the points recorded since the last call.
  When the UI does not drain, the oldest points are dropped and
  `XYFrame::m_trail_dropped` counts them. The audio output is unaffected.
- **Path.** After a recording finishes, `service()` publishes the lane and
  the version bump tells the UI to replace its trail with the authoritative
  path: `read_path(v, f)`.

## Threading

| Call | Thread | Notes |
|---|---|---|
| ctor, dtor, `route*`, `unroute*`, `prepare` | message | add or remove sources and routings (schedule rebuild); the dtor waits on RCU |
| `set_route_enabled`, `set_route_depth`, `XYModeRouter::set_mode` | message | lock-free for audio, no rebuild |
| `touch`, `touch_voice`, `release`, `release_all`, `flush` | one UI thread | lock-free, no allocation |
| `read_frame`, `drain_trail`, `read_path` | one UI thread | wait-free / lock-free / RCU read |
| `set_latch`, `set_voice_enabled`, `set_ui_attached` | any | atomics |
| `recorder(v)` commands, `load_lane`, `clear`, `service` | as `MotionRecorder` | |
| `set_transport`, `process_block`, `reset`, `out_*` | audio | `TANH_NONBLOCKING_FUNCTION`, RTSan-tested |

## Memory and cost

The figures below use the default recorder capacity (32768 points).

| Part | Size |
|---|---|
| Fixed, per controller (trail queue 48 KiB, triple buffer 2.3 KiB) | 51 KiB |
| Fixed, per voice (pad queue and slots, recorder state, 3 outputs) | 14 KiB |
| Per voice, block buffers | about 60 bytes per sample of the maximum block |
| Per voice, recorder take buffers (`MotionRecorderConfig::m_max_points`) | 576 KiB |

A one-voice controller therefore needs about 0.66 MiB, and an 8-voice Single
controller about 5 MiB. Lower `m_recorder.m_max_points` to reduce this.

Measured with `benchmark_modulation --benchmark_filter=bm_xy
--benchmark_repetitions=5` (release, Apple Silicon, machine under load, median
CPU time):

| Block (48 kHz) | 8 controllers alone | full matrix |
|---|---|---|
| 32 | 2.2 µs | 7.5 µs |
| 128 | 8.0 µs | 23.6 µs |
| 256 | 15.7 µs | 46.0 µs |
| 512 | 31.7 µs | 91.2 µs |

- **8 controllers alone**: 8 PerEffect controllers, 6 playing motion and 2
  touched.
- **Full matrix**: the 8 controllers plus a Single controller with 8 voices,
  24 targets, 16 of them with two Replace writers.

At 128 samples, the controllers alone take 0.3 % of the block period and the
whole matrix about 0.9 %.

## Extending: a sequencer layer

A step sequencer would be a third layer below motion. It would be added in the
driver step as follows:

1. Advance the sequencer after `recorder.process()`.
2. Fill the samples where neither touch nor motion is active.
3. Report them as `XYLayer::Sequencer`, appended after `Motion`.
4. Add a pattern version to `XYVoiceFrame`.

None of this changes the matrix side or the UI contract.
