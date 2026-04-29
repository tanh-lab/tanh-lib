# tanh-lib

Modular C++ audio library with four independently buildable components:

- **tanh_core** -- Dispatcher, threading utilities
- **tanh_state** -- Parameter/state management with RCU lock-free reads
- **tanh_dsp** -- DSP utilities, resonator models, effects
- **tanh_audio_io** -- Audio device abstraction over miniaudio

## Build

Requires CMake 3.15+ and a C++20 compiler.

```bash
cmake . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

### CMake options

| Option | Default | Description |
|---|---|---|
| `BUILD_SHARED_LIBS` | ON | Build as shared libraries |
| `TANH_WITH_TESTS` | ON | Build test targets |
| `TANH_BUILD_CORE` | ON | Build Core component |
| `TANH_BUILD_STATE` | ON | Build State component |
| `TANH_BUILD_DSP` | ON | Build DSP component |
| `TANH_BUILD_AUDIO_IO` | ON | Build AudioIO component |
| `TANH_WITH_RTSAN` | OFF | Enable RealtimeSanitizer (requires clang 20) |

When consumed as a subdirectory, disable tests and docs:

```cmake
set(TANH_WITH_TESTS OFF)
set(TANH_WITH_DOCS OFF)
add_subdirectory(modules/tanh-lib)
```

## Testing

```bash
cmake . -B build -DCMAKE_BUILD_TYPE=Debug -DTANH_WITH_TESTS=ON
cmake --build build
```

Run all DSP tests:

```bash
./build/test/dsp/test_dsp
```

### Rings reference fixtures

The Rings resonator tests compare output against reference data generated from
the original Mutable Instruments code. Without fixtures, these tests are
skipped (the build still succeeds).

To generate fixtures (requires SSH access to the `tanh-lab/mutable-instrument-api` repo):

```bash
./test/dsp/generate_reference_fixtures.sh
```

This clones the upstream repo, builds the reference generators, and writes
`.bin` fixtures to `test/dsp/fixtures/`. Then rebuild and run:

```bash
cmake --build build --target test_dsp
./build/test/dsp/test_dsp
```

The fixture files are not checked into version control.

## InputEventQueue and event spreading

`InputEventQueue` (in `tanh_modulation`) is a composable utility for
UI-driven modulators — XYPads, touch-LFOs, MIDI-CC-fed remote controls, and
similar. It is **not a base class**: a source inherits directly from
`ModulationSource` and holds an `InputEventQueue` as a member. The UI
thread pushes discrete events (`push_mono_*`, `push_voice_*`) into a
lock-free SPSC ring buffer owned by the queue; the audio thread drains
the buffer once per block from the source's `pre_process_block()`
override, which `ModulationMatrix::process()` calls on every registered
source before walking the schedule.

The queue's constructor mirrors `ModulationSource`'s own capability flags
(`has_mono`, `num_voices`) so one type serves mono-only, voice-only, and
mono+voice input sources. It carries *no* per-voice value/active cache —
`ModulationSource`'s existing `voice_output(v)` and active-mask buffers
are the single source of truth; the composing source writes into them
directly from the drain callback.

Composition — rather than an `InputModulationSource` base — keeps the
inheritance chain flat (`XYPadSource → ModulationSource`), decouples the
transport from `ModulationSource`'s surface, and lets a source compose
more than one queue if it ever needs separate event streams.

### Why the drain lives in a separate hook

The matrix drains all sources first, then runs the schedule. This keeps
cyclic SCCs (two sources modulating each other) safe: the drained input
state is frozen for the whole block, so multiple cycle iterations never
re-apply the same queued events or see mid-iteration shifts in the cached
value.

Procedural sources (LFOs, envelopes) inherit `ModulationSource` directly
and don't override `pre_process_block()` — it's a virtual no-op by
default, so they pay a single empty dispatch per block.

### Event spreading — per stream, not global

UI events are stamped on the UI thread's wall-clock but can only be drained
on the next audio callback. At drain time, `steady_clock::now()` on the
audio thread is already past every queued event's push time: there is no
honest way to recover a sub-block offset from a UI-sourced event.

The naive fix — collapse all drained events to offset 0 with "last-value
wins" — breaks the quick-on/off case: a tap where `Active(true)` and
`Active(false)` both arrive in the same block ends up with the final
cached state `active=false`, and the downstream gate never fires.

Instead, `drain_spread()` buckets events by stream (one bucket for mono,
one per voice) and **spreads within each bucket independently**. The i-th
of `n_bucket` events in a bucket is placed at
`offset = i * block_size / n_bucket`. FIFO ordering is preserved per
stream; cross-stream ordering in the queue is intentionally discarded
because streams are independent by construction.

Per-stream rather than global spread is essential for polyphony. 10
simultaneous touch-down events on 10 distinct voices each have
`n_bucket = 1` in their own bucket, so all 10 land at offset 0 — a true
chord trigger, not a 460-sample strum across the block.

For the tap example on voice 0 in a 512-sample block with events
`[Active(true), Value(0.5), Active(false)]`:

- voice-0 bucket has 3 events → offsets `[0, 170, 341]`
- voice-0 `voice_output` buffer: carry-over across `[0, 170)`, `0.5`
  across `[170, 512)`
- voice-0 active mask: `1` across `[0, 341)`, `0` across `[341, 512)`
- downstream `CombineMode::Replace` on `play` fires `note_on` at sample 0
  and `note_off` at sample 341 — the transient tap is audible

### What this does and doesn't claim

- **Transition ordering is preserved per stream.** Rising and falling
  edges register as distinct change points, so gate-style routings
  (`Replace`, `ReplaceHold`) don't lose edges.
- **Polyphonic simultaneity is preserved.** Events on different voices
  (or mono vs voice) don't strum each other — each stream spreads in
  isolation.
- **Offsets are fabricated, not measured.** A tap that really happened at
  samples 50 and 200 is placed at 0 and 256. Audibly, the gate is a tap
  of different duration than the user produced, within the block.
- **Latency is up to one block.** Events pushed during block N drain at
  the top of block N+1.
- **Not a substitute for driver-timestamped events.** MIDI and any
  tightly-timed control source need a transport that shares the audio
  thread's time domain. `InputEventQueue` does not.

### Composing a source

A typical source is ~25 lines — 10-voice XYPad axis source:

```cpp
class XYPadSource : public thl::modulation::ModulationSource {
public:
    XYPadSource()
        : ModulationSource(/*has_mono=*/false,
                           /*num_voices=*/10,
                           /*fully_active=*/false),
          m_queue(/*has_mono=*/false,
                  /*num_voices=*/10,
                  /*queue_capacity=*/80) {}

    void prepare(double, size_t block_size) override {
        resize_buffers(block_size);
        m_queue.prepare();
    }

    // UI thread.
    bool push_value(uint32_t v, float val)  { return m_queue.push_voice_value(v, val); }
    bool push_active(uint32_t v, bool a)    { return m_queue.push_voice_active(v, a); }

    // Audio thread — block boundary. Fills voice_output + active mask directly
    // from events. No intermediate segment storage.
    void pre_process_block() override {
        const uint32_t bs = current_block_size();

        // Seed each voice with its carry-over (previous block's final sample).
        for (uint32_t v = 0; v < num_voices(); ++v) {
            float carry = voice_output(v)[bs - 1];
            std::fill_n(voice_output(v), bs, carry);
        }

        m_queue.drain_spread(bs, [this, bs](const auto& e, uint32_t offset) {
            if (e.type == thl::modulation::InputEventQueue::EventType::Value) {
                // Overwrite [offset..bs); later events in this bucket overwrite their tails.
                std::fill(voice_output(e.voice) + offset,
                          voice_output(e.voice) + bs, e.value);
            } else if (e.active) {
                set_voice_output_active(e.voice, offset);
            }
            record_voice_change_point(e.voice, offset);
        });
    }

    // process_voice() is a no-op — pre_process_block already filled voice_output.

private:
    thl::modulation::InputEventQueue m_queue;
};
```

Any future input-modulator type (touch-LFO, haptic, MIDI-CC) repeats
this template. Mono-only sources construct
`InputEventQueue(true, 0, cap)` and use `push_mono_*` /
`set_output_active(...)`; mixed mono+voice sources construct
`InputEventQueue(true, N, cap)` and dispatch whichever bucket each event
came from.

## Android Bluetooth SCO (HFP) Notes

Bluetooth SCO (used by the HFP profile) is a synchronous bidirectional codec.
Unlike A2DP (output-only, high quality), SCO carries both playback and capture
in a single link.  Getting audio to flow requires several steps that are easy
to miss:

1. **Permissions** — the app manifest must declare `BLUETOOTH`, `BLUETOOTH_CONNECT`
   (runtime permission on API 31+), and `MODIFY_AUDIO_SETTINGS`.
2. **Capture must run alongside playback** — the SCO codec does not fully
   negotiate the bidirectional channel until both an output and input stream
   are active.  Call `start_capture()` immediately after `start_playback()` when
   the selected input device is a SCO device.

## Android Bluetooth SCO Capture — Sample Rate Bug

On Android, when recording via Bluetooth SCO (HFP profile), the actual audio
codec rate is **16 kHz (mSBC)**, **8 kHz (CVSD)**, or **32 kHz (LC3-SWB,
HFP 1.9+)**. However, every public
Android API incorrectly reports **48 000 Hz** (the system mixer rate):

| API | Returns | Actual |
|-----|---------|--------|
| `AAudioStream_getSampleRate()` | 48 000 | 8 000 / 16 000 / 32 000 |
| `AudioDeviceInfo.getSampleRates()` | [48 000] | 8 000 / 16 000 / 32 000 |
| `AudioRecordingConfiguration.getFormat().getSampleRate()` | 48 000 | 8 000 / 16 000 / 32 000 |

This is an **undocumented platform behavior** — the Android audio HAL
behaviour is **device-specific**: some HALs silently deliver raw codec-rate
frames labelled as 48 kHz, while others genuinely resample to 48 kHz before
the app receives them. No public API can distinguish the two cases.

### Our workaround — callback-based rate measurement

Because Android APIs lie about the SCO capture rate, and the actual behaviour
differs between devices, we **measure the real delivery rate at runtime** by
timing capture callbacks:

1. When `start_capture()` is called, three atomics are reset
   (`m_capture_first_callback_us`, `m_capture_total_frames`, `m_capture_measured_rate`).
2. In `process_callbacks()`, the first capture callback records a timestamp.
   Subsequent callbacks accumulate a frame count. After
   `k_rate_calibration_frames` (16 000) frames the elapsed wall-clock time is
   used to compute the actual rate, which is snapped to the nearest standard
   rate (8 000 / 16 000 / 32 000 / 48 000 Hz).
3. `wait_for_capture_rate_measurement(timeoutMs)` blocks until the measurement
   completes (or times out). The recording flow calls this before opening the
   WAV file so the header sample rate is correct.
4. `get_capture_sample_rate()` returns the measured rate when available; otherwise
   it falls back to the API-reported rate.

This correctly handles both device types:
- Devices whose HAL delivers 16 kHz data labelled as 48 kHz → measurement
  yields **16 000 Hz**.
- Devices whose HAL genuinely resamples to 48 kHz → measurement yields
  **48 000 Hz**.

### Duplex capture

Currently, SCO recording uses a **standalone capture device** (not duplex).
If duplex capture via SCO is ever needed (e.g. real-time monitoring or
effects), the capture side will deliver audio at the real codec rate
(8/16/32 kHz) while the playback side runs at 48 kHz. In that case the capture
audio must be **resampled to the playback sample rate** before mixing or
processing in the duplex callback.

### Relevant source

- `modules/tanh-lib/src/audio-io/AudioDeviceManager.cpp` — `process_callbacks()` (rate measurement), `get_capture_sample_rate()`, `wait_for_capture_rate_measurement()`
- `native/jsi/src/NativeCosmosJSI.cpp` — `startRecording` calls `wait_for_capture_rate_measurement()` then `get_capture_sample_rate()`

### References

- Android `startBluetoothSco()` docs state SCO input sampling "must be 8 kHz"
  (predates mSBC; modern headsets use 16 kHz)
- Confirmed on Samsung SM-X716B, Android API 36 (March 2026)
