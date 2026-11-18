# Sampler: play head and looper

`thl::dsp::sampler` (`include/tanh/dsp/sampler/`) plays a region of a sample
with one continuous, interpolating play head. It knows nothing about grains,
parameter systems or sample stores: it reads `SampleView`s the host hands it
and writes planar audio.

| Header | What it gives you |
|---|---|
| `SampleView.h` | Non-owning planar view of a sample; `read_clamped` / `read_wrapped` interpolated reads |
| `LoopRegion.h` | Start / End / Loop as a region with a direction |
| `LoopMarkers.h` | Markers to region: minimum span, zero-crossing snap, cache |
| `ZeroCrossing.h` | Upward zero-crossing test and nearest-crossing search |
| `LoopCrossfade.h` | Quarter-sine fade table and the loop-fade plan |
| `SamplePlayer.h` | The player / looper built from the above |

## Using the player

```cpp
#include <tanh/dsp/sampler/SamplePlayer.h>

thl::dsp::sampler::SamplePlayer player;
player.prepare(sample_rate);            // allocates: setup thread
player.note_on();

// audio thread, per block
const auto source = thl::dsp::sampler::SampleView::of(buffer);
player.set_markers(start, end, loop);   // normalised
player.set_speed(1.0f);
player.set_loop(true);
player.set_snap(true);
player.render({&source, 1}, 0, outputs, num_channels, num_frames);
```

`render()` writes the source's own channels (a mono source feeds every output)
and overwrites the output. Channel mapping such as mid/side width is the
caller's job; the granular voice does it in `granular::channel_mixer::mix_head`.

## Regions and direction

A `LoopRegion` stores its bounds ascending and a `m_reverse` flag. The head runs
in *virtual* forward coordinates and only the frame that is read is mirrored
(`physical = start + end - 1 - virtual`). End before Start therefore plays
backwards with no second code path. When the markers move under a reversed head,
the player re-expresses the head against the new region, so the audible
position never jumps. Moving a marker never moves the head in either direction.

A Loop marker outside the region, or parked on its exit, loops the whole region
from its entry. A UI that clamps Loop into [Start, End] parks it on End when
the markers are squeezed together, and that must not leave a one-frame loop.

`set_region()` bypasses markers entirely, for hosts that resolve regions
themselves (e.g. from a `slicing::SliceMap`, see [slicing](slicing.md)).

## Loop crossfade

A wrap from End to Loop is a jump in the waveform. The player hides it the way a
hardware sampler does:

- **Before End** (the default): over the last stretch before End the head is
  blended into a copy reading the audio just before Loop. At End the head is
  already reading what follows Loop, so the jump is silent and nothing past End
  is read.
- **After the wrap** (when there is more room past End than before Loop, e.g.
  Loop at the sample's start): the head starts at Loop under a copy that carries
  on past End and fades out.

Each fade is at most half the loop body and at most `m_crossfade_seconds` of
playback. The minimum loop body (`m_min_loop_seconds`) keeps it from shrinking
to nothing. While a fade runs the region is held and marker moves land at the
wrap.

The fade is equal-power, except when both ends of the jump sit on upward zero
crossings (snap on and a crossing found). The two reads are then in phase, and
equal-power would add +3 dB mid-fade, so the gains sum to one instead.

## Zero-crossing snap

With snap on, `LoopMarkers` moves Start, End and Loop to the nearest upward zero
crossing (sum of the first two channels) within `m_snap_radius_seconds`, and
leaves a marker where it is when none is in reach. The direction always comes
from the raw markers, and End never snaps back inside the minimum span. Results
are cached, so the search only runs when a marker moves.

Snapping gives a click-free join, not a pitch-true one: a very short loop sounds
at a pitch set by its length, and the next crossing is not necessarily one
period away.

## Sources and discontinuities

`render()` takes a span of alternative sources and the index to play. Sources
must be equal-length and time-aligned (see [pitch banks](pitch.md)). Switching
keeps the head position and crossfades. A retrigger, a source switch, End moved
behind the head and a one-shot reaching End all park a copy of the head in a
pool of three outgoing heads. Those fade out while the live head fades back in;
a fourth discontinuity inside a fade steals the quietest tail.

## Real-time safety

`prepare()` allocates. `render()`, the setters and the getters are allocation-free
and lock-free. The host keeps the buffers behind every `SampleView` alive while
the audio thread may read them.
