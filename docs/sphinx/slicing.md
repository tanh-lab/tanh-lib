# Slicing

`thl::dsp::slicing` (`include/tanh/dsp/slicing/`) cuts a sample into slices and
turns slices into playable regions.

| Header | What it gives you |
|---|---|
| `SliceMap.h` | Slice boundaries in frames; `grid`, `from_normalized`, `from_frames`, `valid_normalized_bounds` |
| `TransientSlicer.h` | Offline transient detection that picks N slices |
| `SliceSteps.h` | Optional "slice space" mapping of 0..1 controls onto slices |

## SliceMap

A `SliceMap` holds up to `k_max_slices` (64) slices as frame boundaries of a
sample `m_total_frames` long: `m_bounds[0] == 0`, `m_bounds[m_count] == total`,
strictly ascending. It is trivially copyable with a fixed capacity, so a host can
hand a whole map to the audio thread by value (a seqlock, an atomic swap of a
copy, a message queue). tanh-lib does not prescribe the handoff.

A map belongs to a sample length, not to one buffer. Alternative sources of the
same length (pitch-shifted copies, round-robins) share it, and `boundary_frame()`
rescales for a source of another length.

Hosts that store or edit boundaries as fractions (a UI, a preset) convert with
`from_normalized()` / `normalized_bounds()` and check edits with
`valid_normalized_bounds()`, which takes the host's own slice-count range and
minimum slice length.

## TransientSlicer

`TransientSlicer` is offline analysis for a loader or worker thread; it
allocates.

1. `analyse(buffer, sample_rate)` computes an onset-strength curve: mono
   mixdown, a low / mid / high split (160 Hz and 1.6 kHz by default), RMS per hop
   over a trailing window, log compression, positive first difference summed
   over the bands, local mean subtracted.
2. `pick(analysis, count, &buffer)` keeps the `count - 1` strongest peaks at
   least `m_min_slice_seconds` apart. With the audio passed, each boundary is
   refined: moved to the sharpest energy rise, back to the quietest frame before
   the attack, then onto a zero crossing. With fewer peaks than needed the
   longest gaps are split evenly, and silence gives the grid.

Every threshold is in `TransientSlicer::Settings`. Picking a different count
only re-runs `pick()`.

## Slice space

`SliceSteps.h` implements one way to drive slices from continuous controls.
With N slices, a position-like value `u` picks slice `floor(u * N)` and a
marker-like value picks boundary `round(u * N)`, however long each slice is. A
pad, an LFO or a sequencer sweeping the control therefore steps through slices
evenly. `region_from_steps(map, start, end, total)` turns two marker values into
a `sampler::LoopRegion`: the slices between them, reversed when End is before
Start, one slice when both pick the same boundary. `granular::PositionSprayHead`
uses the same mapping for its Spray / Tilt window.
