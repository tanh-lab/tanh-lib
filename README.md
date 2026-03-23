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

## Android Bluetooth SCO (HFP) Notes

Bluetooth SCO (used by the HFP profile) is a synchronous bidirectional codec.
Unlike A2DP (output-only, high quality), SCO carries both playback and capture
in a single link.  Getting audio to flow requires several steps that are easy
to miss:

1. **Permissions** — the app manifest must declare `BLUETOOTH`, `BLUETOOTH_CONNECT`
   (runtime permission on API 31+), and `MODIFY_AUDIO_SETTINGS`.
2. **Capture must run alongside playback** — the SCO codec does not fully
   negotiate the bidirectional channel until both an output and input stream
   are active.  Call `startCapture()` immediately after `startPlayback()` when
   the selected input device is a SCO device.

## Android Bluetooth SCO Capture — Sample Rate Bug

On Android, when recording via Bluetooth SCO (HFP profile), the actual audio
codec rate is **16 kHz (mSBC)** or **8 kHz (CVSD)**. However, every public
Android API incorrectly reports **48 000 Hz** (the system mixer rate):

| API | Returns | Actual |
|-----|---------|--------|
| `AAudioStream_getSampleRate()` | 48 000 | 8 000 / 16 000 |
| `AudioDeviceInfo.getSampleRates()` | [48 000] | 8 000 / 16 000 |
| `AudioRecordingConfiguration.getFormat().getSampleRate()` | 48 000 | 8 000 / 16 000 |

This is an **undocumented platform behavior** — the Android audio HAL silently
resamples SCO capture from the real codec rate to the mixer rate before any
public API can observe it. This is not filed as a bug on the Google Issue
Tracker, and no workaround exists to query the true rate.

### Our workaround

`getAndroidScoSampleRate()` in `AndroidAudioDevices.cpp` clamps any reported
rate above 16 kHz down to **16 000 Hz** (`kMaxScoRate`), since no SCO codec
operates above 16 kHz. `getCaptureSampleRate()` in `AudioDeviceManager.cpp`
applies this correction only when:

1. The SCO link is globally enabled (`isAndroidBluetoothScoEnabled()`), **and**
2. The currently selected input device is a Bluetooth SCO device.

This ensures WAV files are written with the correct sample rate header.

### Duplex capture

Currently, SCO recording uses a **standalone capture device** (not duplex).
If duplex capture via SCO is ever needed (e.g. real-time monitoring or
effects), the capture side will deliver audio at the real codec rate
(8/16 kHz) while the playback side runs at 48 kHz. In that case the capture
audio must be **resampled to the playback sample rate** before mixing or
processing in the duplex callback.

### Relevant source

- `modules/tanh-lib/src/audio_io/AndroidAudioDevices.cpp` — `getAndroidScoSampleRate()`
- `modules/tanh-lib/src/audio_io/AudioDeviceManager.cpp` — `getCaptureSampleRate()`
- `native/jsi/src/NativeCosmosJSI.cpp` — `startRecording` uses `getCaptureSampleRate()`

### References

- Android `startBluetoothSco()` docs state SCO input sampling "must be 8 kHz"
  (predates mSBC; modern headsets use 16 kHz)
- Confirmed on Samsung SM-X716B, Android API 36 (March 2026)
