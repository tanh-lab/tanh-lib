# Recording takes

Recording several signals at once, so that the files line up to the sample: every engine,
every return channel and the mix. Three parts, each usable alone:

| Part | Component | Does |
| --- | --- | --- |
| `dsp::transport::RecordSchedule` | DSP (header-only) | Decides which samples belong to a take, so it starts and ends on a bar line |
| `audio_io::MultitrackRecorder` | AudioIO | Moves the tracks from the audio thread to one WAV file each |
| `audio_io::export_mixdown()`, `export_track()`, `export_tracks()` | AudioIO | Turn a finished take into the files a user takes away |

The recorder does not know the transport and the schedule does not know files; the
application passes a few numbers from one to the other.

## One block

```cpp
// audio thread
const auto window = schedule.advance(transport);           // TransportInfo of this block
recorder.begin_block(num_samples,
                     {.m_begin = window.m_begin, .m_end = window.m_end, .m_final = window.m_final});
recorder.write(0, engine_a);                               // wherever each signal is at hand
recorder.write(1, engine_b);
recorder.write(2, return_a);
recorder.write(3, mix);
if (window.m_final) {
    const auto& t = schedule.timing();
    recorder.set_timing({.m_bpm = t.m_bpm, .m_sig_num = t.m_sig_num, .m_sig_denom = t.m_sig_denom,
                         .m_start_beat = t.m_start_beat, .m_length_beats = t.m_length_beats,
                         .m_length_frames = t.m_length_frames});
}
recorder.end_block();
```

Call these every block, whether a take is open or not: without one they do nothing. None of
them allocates, locks or touches the file system.

A track nobody wrote in a block gets silence for it, and channels a block lacks are silent,
so the tracks have the same length whatever happens.

## One take

```cpp
// control thread
recorder.prepare({.m_sample_rate = 48000.0, .m_max_block_size = 512,
                  .m_tracks = {{"Engine 1", 2}, {"Engine 2", 2}, {"Return A", 2},
                               {"Mix", 2, audio_io::TrackRole::Mix}}});
schedule.prepare(48000.0);

recorder.begin_take(directory, audio_io::SampleFormat::Pcm24);   // files exist, waiting
schedule.arm();                                                  // starts on the next bar line
// ...
schedule.stop();                                                 // runs to the end of the bar
// recorder.state() == RecorderState::Done: recorder.last_take()
```

`prepare()` names every track that can be recorded and reserves its buffer (about 1 MB for a
stereo track at 48 kHz). Which of them a take records is chosen per take, with nothing
prepared again:

```cpp
recorder.begin_take(directory, format);            // every track
recorder.begin_take(directory, format, {3});       // only the mix
recorder.begin_take(directory, format, {0, 1, 2}); // only the stems
```

The audio thread hands over every track in every block either way; tracks a take does not
record are ignored there and get no file, so the choice decides what a take costs on disk.

`begin_take()` creates the directory and one file per recorded track. The take ends at the sample a
span marks as final; the recorder's writer thread then writes what is left, closes the files
and writes the manifest. `state()` says where that stands, and a callback
(`set_take_callback()`) runs on the writer thread when the take is on disk.

`end_take()` ends a take from the control thread instead, with what has arrived: for a stop
that is not on a grid, or an audio device that went away. `cancel_take()` drops the take,
also one whose end is still being written, and removes its files; files that were in the
directory before the take stay.

## Whole bars

`RecordSchedule` makes every take a whole number of bars:

- `arm()` waits for the next bar line and starts there, at its sample inside the block.
- `stop()` lets the take run to the end of its bar. `arm(n)` ends it after `n` bars by
  itself. The shortest take is one bar.
- Bars are counted in beats of the transport, not in samples. A tempo change during the take
  still gives whole bars, and a host loop that wraps does not disturb the count.
- `set_tail_seconds()` keeps recording after the last bar, so reverb and delay ring out. The
  timing keeps the musical length (`m_length_frames`) apart from the tail, so the take still
  loops: `Take::musical_frames()` is the length to export for a loop.

`status()` is what a display reads, from any thread:

| Field | While | Says |
| --- | --- | --- |
| `m_phase` | always | `Idle`, `Armed`, `Recording`, `Finishing`, `Tail` |
| `m_beats_until_start` | `Armed` | Count-in to the bar line |
| `m_elapsed_beats`, `elapsed_bars()` | recording | Musical time so far |
| `m_length_beats`, `length_bars()` | once known | The length the take will have (`arm(n)`, or after `stop()`) |
| `m_beats_left` | once known | Beats to the end |
| `m_takes_finished` | always | Counts up when a take ends |

Off the grid: when the transport is not running as `arm()` takes effect (or with
`set_grid_enabled(false)`), the take starts at once, `stop()` ends it at once, and it has no
musical length. A take on the grid ends where the transport stops; its length is then not a
whole number of bars. A timeline that starts over while playing (a Link session is joined) is
not a stop: the take keeps counting its bars.

## What is on disk

A take is a directory:

```text
take-0007/
  Engine 1.wav
  Engine 2.wav
  Return A.wav
  Mix.wav
  take.json
```

Every file has the same length and starts on the same sample. Track names become file names
with unsafe characters replaced; the manifest keeps the names as given.

```json
{
  "version": 1,
  "sample_rate": 48000.0,
  "format": "pcm24",
  "num_frames": 408000,
  "dropped_frames": 0,
  "truncated": false,
  "timing": {
    "bpm": 120.0, "sig_num": 4, "sig_denom": 4,
    "start_beat": 16.0, "length_beats": 16.0, "length_bars": 4.0, "length_frames": 384000
  },
  "tracks": [
    {"name": "Engine 1", "file": "Engine 1.wav", "channels": 2, "role": "stem"},
    {"name": "Mix", "file": "Mix.wav", "channels": 2, "role": "mix"}
  ]
}
```

`load_take(directory)` reads it back in a later session. A directory without a manifest is a
take that did not finish.

Files are written under a temporary name and appear under their own when they are complete.

Formats: 16-bit and 24-bit integer and 32-bit float WAV. A stereo track at 48 kHz takes about
23 MB a minute in float, 17 MB in 24-bit and 12 MB in 16-bit.

## Mixdown and export

Two mixes exist:

- **The mix as it was heard**: record it as a track with `TrackRole::Mix`. It includes
  everything after the stems (master effects, the limiter) and is there when the take ends.
- **A mix from the stems**: `export_mixdown()` sums the `Stem` tracks with a gain per track,
  so the balance can be changed afterwards. It equals the recorded mix only when nothing
  non-linear sits between the stems and the output.

```cpp
audio_io::ExportOptions options;
options.m_format = audio_io::SampleFormat::Pcm16;
options.m_num_frames = take.musical_frames();        // without the tail: a file that loops
options.m_track_gains = {1.0f, 0.5f, 0.0f};          // by track index; 0 mutes
options.m_normalize_db = -1.0f;
options.m_progress = [&](float part) { return !cancelled; };

audio_io::export_mixdown(take, "mix.wav", options);  // the stems, summed
audio_io::export_track(take, 3, "heard.wav", options);   // one track as recorded
audio_io::export_tracks(take, "stems/", options);    // every track, same range
```

The exports read and write block by block, publish a file only when it is complete, and
block: run them on a worker thread. `m_progress` reports 0 to 1 and cancels by returning
false; a cancelled or failed export leaves its directory as it was, files from an earlier
export included. `export_tracks()` refuses the take's own directory.

Only WAV is written. Compressed formats need an encoder the library does not carry.

## When things go wrong

| Situation | What happens |
| --- | --- |
| The disk stalls longer than the recorder's buffer (`m_buffer_seconds`, two seconds) | The blocks without room are left out of every track and written as silence in every file. The take keeps its length and alignment; `dropped_frames()` and the manifest count them |
| A file reaches 4 GiB (the WAV limit, about three hours of stereo float at 48 kHz) | The take ends there and is marked `truncated` |
| A write fails (disk full) | The take is `Failed` and its files are removed |
| Only the manifest cannot be written | The take is `Failed`, the audio files are complete and stay |
| A block takes very long on the audio thread (an offline render, a debugger) | The writer thread waits for it; nothing is lost |
| The audio device stops during a take | `end_take()` keeps what arrived |

## Recording one signal

`AudioFileSink` records one interleaved signal from an `AudioIODeviceCallback` to one file.
Its `process()` copies into a buffer and a thread of the sink writes the file, so it is safe
on the audio thread as well. `close_file()` returns whether the file is complete at its path;
after a failed write (a full disk) it is not published and `has_failed()` is true.

## Threading

| Call | Thread |
| --- | --- |
| `MultitrackRecorder::prepare()`, `RecordSchedule::prepare()` | Any, with the audio stopped |
| `begin_block()`, `write()`, `set_timing()`, `end_block()`, `RecordSchedule::advance()`, `timing()` | Audio |
| `begin_take()`, `end_take()`, `cancel_take()`, `last_take()` | One control thread |
| `state()`, `frames_recorded()`, `dropped_frames()`, `RecordSchedule::arm()`, `stop()`, `cancel()`, `status()` | Any |
| The take callback | The recorder's writer thread; do not start a take from it |
| `export_*()`, `load_take()`, `save_take()` | Any that may block |

## Tests

`WavStreamWriter.*`, `MultitrackRecorder.*`, `Take.*`, `TakeExport.*` and `RecordedTake.*` in
`test_audio_io`, `RecordSchedule.*` in `test_dsp`.
