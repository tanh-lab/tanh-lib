# AudioIO: Android notes

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

- `src/audio-io/AudioDeviceManager.cpp` — `process_callbacks()` (rate measurement), `get_capture_sample_rate()`, `wait_for_capture_rate_measurement()`
- A recording flow calls `wait_for_capture_rate_measurement()` and then
  `get_capture_sample_rate()` before it writes the file header

### References

- Android `startBluetoothSco()` docs state SCO input sampling "must be 8 kHz"
  (predates mSBC; modern headsets use 16 kHz)
- Confirmed on Samsung SM-X716B, Android API 36 (March 2026)

