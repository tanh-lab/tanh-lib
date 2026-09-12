# Modulation: input events


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

## Why the drain lives in a separate hook

The matrix drains all sources first, then runs the schedule. This keeps
cyclic SCCs (two sources modulating each other) safe: the drained input
state is frozen for the whole block, so multiple cycle iterations never
re-apply the same queued events or see mid-iteration shifts in the cached
value.

Procedural sources (LFOs, envelopes) inherit `ModulationSource` directly
and don't override `pre_process_block()` — it's a virtual no-op by
default, so they pay a single empty dispatch per block.

## Event spreading — per stream, not global

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

## What this does and doesn't claim

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

## Composing a source

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

