# Pitch banks

`thl::dsp::pitch::PitchBank` (`include/tanh/dsp/pitch/PitchBank.h`) builds
pitch-shifted copies of a sample, one slot per semitone. A player then changes
pitch by switching source instead of resampling on the audio thread.

```cpp
thl::dsp::pitch::PitchBank const bank;             // -24..+24 semitones
std::vector<thl::core::BufferF> slots(bank.num_slots());
slots[bank.root_index()] = std::move(root);        // decoded sample
const std::vector<int> semitones = {-12, 0, 7, 12};
bank.build(slots, semitones, sample_rate);         // loader thread: seconds, allocates
```

Every copy keeps the root's length and timing: it is a pitch shift, not a
resample. The slots are therefore equal-length and time-aligned, which is the
contract `sampler::SamplePlayer` and `granular::GrainEngine` need to switch
between them mid-play without moving the head.

Shifting uses [Signalsmith Stretch](https://github.com/Signalsmith-Audio/signalsmith-stretch)
(MIT, cheaper preset, tonality limit `m_tonality_limit_hz`). Each copy gets short
linear fades at both edges (`m_edge_fade_frames`), and the work is spread across
`m_max_threads` workers (default: half the hardware threads).
`m_copy_only` fills the slots with plain copies, for debugging and tests.

Signalsmith Stretch is fetched at configure time and linked privately. Its
headers and templates stay inside `PitchBank.cpp`: nothing of it is exported
(`test/exports` forbids the `signalsmith` namespace) or installed.
