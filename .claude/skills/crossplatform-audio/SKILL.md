---
name: crossplatform-audio
description: >
  Cross-platform real-time audio development guide covering C++ DSP cores with
  platform-specific backends (AAudio/miniaudio on Android, CoreAudio/AVAudioSession
  on iOS, PipeWire/ALSA on Linux). Use this skill whenever the user is working on
  audio callback code, buffer sizing, device routing, Bluetooth audio profiles
  (A2DP/HFP/SCO), audio session configuration, lock-free audio↔UI communication,
  or debugging silence/glitch/latency issues on any platform. Also trigger when the
  user mentions miniaudio, Oboe, AAudio, CoreAudio, AVAudioSession, PipeWire,
  WirePlumber, AUv3, audio interruptions, sample rate mismatches, or real-time
  safety constraints. Even if the question seems simple ("why is there silence on
  Bluetooth?"), use this skill — audio platform bugs are rarely simple.
---

# Cross-Platform Real-Time Audio Development

This skill encodes hard-won knowledge about building real-time audio applications
that target iOS, Android, and Linux from a shared C++ DSP core.

## When to read the reference files

This SKILL.md contains the universal rules and architecture overview.
Platform-specific details live in `references/`:

- **`references/android.md`** — AAudio, miniaudio Android backend, JNI audio device
  enumeration, Samsung quirks, Bluetooth SCO on Android
- **`references/ios.md`** — CoreAudio, AVAudioSession categories, AUv3 constraints,
  Bluetooth HFP/A2DP switching, buffer size ceilings
- **`references/linux.md`** — PipeWire, WirePlumber, BlueZ, ALSA, JACK routing,
  headless audio setups

Read the relevant platform file(s) before answering platform-specific questions.
For cross-platform architectural questions, read this file first then consult
platform files as needed.

---

## Core Architecture Pattern

```
┌─────────────────────────────────────────────────┐
│  UI Layer (React Native / CHOC WebView / native) │
│  ── communicates via lock-free structures ──      │
├─────────────────────────────────────────────────┤
│  C++ DSP Core (platform-agnostic)                │
│  • AudioDSPCore class, pure C++                  │
│  • Processes audio in fixed-size blocks           │
│  • No platform headers, no allocations            │
├─────────────────────────────────────────────────┤
│  Platform Audio Backend                           │
│  • Android: AAudio (via miniaudio or Oboe)        │
│  • iOS/macOS: CoreAudio (via miniaudio or direct)  │
│  • Linux: PipeWire / JACK / ALSA                  │
│  • Handles device enumeration, routing, sessions  │
└─────────────────────────────────────────────────┘
```

### Key principle: the DSP core never knows what platform it's on.

Platform backends call into the DSP core with a buffer pointer, frame count,
channel count, and sample rate. The DSP core processes and returns. Everything
platform-specific (session management, device routing, Bluetooth negotiation)
stays in the backend layer.

---

## Real-Time Safety Rules (Audio Thread)

These are **absolute** — violating any of them causes glitches, dropouts, or silence.

### NEVER do on the audio thread:
- **Allocate or free memory** (`new`, `delete`, `malloc`, `free`, `std::vector::push_back`)
- **Lock a mutex** (`std::mutex`, `std::lock_guard`, `pthread_mutex_lock`)
- **Make system calls** (file I/O, logging, `NSLog`, `__android_log_print`)
- **Call Objective-C methods** that might autorelease or message-send through the runtime
- **Call JNI methods** (they can trigger GC)
- **Use condition variables** (`notify_one` involves internal locks and scheduling)
- **Throw exceptions**
- **Call `std::function`** if it might heap-allocate (use function pointers or templates)

### ALWAYS on the audio thread:
- **Pre-allocate everything** during initialization
- **Use lock-free structures** for audio↔UI communication (SPSC ring buffers,
  atomics, RCU patterns)
- **Process in fixed-size blocks** — if the callback delivers a different frame
  count, chunk it yourself
- **Read back granted parameters** — never assume your requested buffer size or
  sample rate was honored

### Lock-free communication patterns:
```
Audio Thread ──SPSC ringbuffer──▶ UI Thread (meter values, waveforms)
UI Thread ──atomic<float>──▶ Audio Thread (parameter changes)
UI Thread ──RCU/double-buffer──▶ Audio Thread (structural changes like new grains)
```

---

## Buffer Size and Callback Chunking

**Problem**: Platform audio callbacks deliver variable frame counts. Your DSP core
expects a fixed block size.

**Solution**: Defensive chunking loop in the callback:

```cpp
void audioCallback(float* output, uint32_t frameCount, uint32_t channels) {
    uint32_t framesProcessed = 0;
    while (framesProcessed < frameCount) {
        uint32_t chunkSize = std::min(frameCount - framesProcessed, preparedBufferSize);
        dspCore.process(output + framesProcessed * channels, chunkSize, channels);
        framesProcessed += chunkSize;
    }
}
```

**Why this matters per platform:**
- **Android (AAudio)**: `framesPerBurst` varies by device (Samsung: 1536, Pixel: 192).
  The HAL determines this, not your code. With `noFixedSizeCallback = false`, miniaudio
  adds an intermediary buffer, but you should still chunk defensively.
- **iOS (CoreAudio)**: Usually delivers the granted `IOBufferDuration` consistently,
  but can vary during session changes, route changes, and startup/shutdown.
- **Linux (PipeWire/JACK)**: Typically fixed per session, but PipeWire's adaptive
  quantum can change it.

---

## Bluetooth Audio Across Platforms

Bluetooth audio involves two fundamentally different profiles:

| Profile | Quality | Mic | Sample Rate | Latency |
|---------|---------|-----|-------------|---------|
| **A2DP** | High (AAC/LDAC/SBC-XQ) | No | 44.1–96 kHz | Higher |
| **HFP/SCO** | Low (CVSD/mSBC) | Yes | 8–16 kHz | Lower |

**You cannot have high-quality output AND mic input simultaneously** (except
AirPods on iOS 26+ with `bluetoothHighQualityRecording`).

### Common Bluetooth bugs:
1. **Silence on large buffer requests** — iOS clamps `setPreferredIOBufferDuration`
   at ~0.093s. Requesting more → silence. SCO at 16kHz with 120-frame packets makes
   large buffers structurally incompatible.
2. **SCO connection retries disrupting A2DP** — BlueZ repeatedly attempting HFP
   Gateway connection ("Unable to get Hands-Free Voice gateway SDP record: Host is
   down") every 60s, causing A2DP glitches. Fix: `Disable=Gateway` in
   `/etc/bluetooth/main.conf`.
3. **Android sample rate mismatch** — Android HAL reports 48kHz to apps but SCO
   operates at 16kHz. Input path often skips resampling while output path handles
   it correctly. Recordings play back at wrong speed.
4. **Profile switching latency** — Switching between A2DP and HFP/SCO takes 1–3
   seconds. Don't do it in response to transient events.

---

## Debugging Playbook

### Silence (no audio output)
1. Check if the audio session/stream is actually running (not just configured)
2. On iOS: verify `AVAudioSession` category and `setActive:` succeeded
3. On Android: check if AAudio stream state is `Started` (not `Disconnected`)
4. Check buffer size — did you request more than the platform supports?
5. On Bluetooth: which profile is active? A2DP vs HFP can cause silent fallback
6. Check miniaudio: look for `WARNING: Outputting silence` in logs (frame count
   mismatch in callback)

### Glitches / Dropouts
1. Check for real-time safety violations (add `-fsanitize=thread` to find them)
2. On Android: is the FAST path being used? Check with `adb shell dumpsys audio`
3. Check CPU load — are you doing too much work per callback?
4. On Linux: check PipeWire quantum and buffer settings in `pw-top`
5. On Bluetooth: check for profile negotiation retries disrupting the stream

### Latency
1. Measure round-trip, not just output latency
2. On Android: `framesPerBurst` × 2 is the practical minimum (double buffering)
3. On iOS: `setPreferredIOBufferDuration` with readback of actual granted value
4. Never trust the "requested" value — always read back what the system granted

### Device Routing Changes
1. On Android: miniaudio's `notificationCallback` with `ma_device_notification_type_rerouted`
2. On iOS: `AVAudioSessionRouteChangeNotification`
3. After routing change: re-enumerate devices, update hardware state, possibly
   restart the stream
4. BT SCO disconnects are more fragile than wired — may need full stream restart

---

## Cross-Platform Testing Strategy

- **Unit test the DSP core** independently with synthetic buffers — no platform
  audio needed
- **Use `CMAKE_CROSSCOMPILING_EMULATOR`** for running iOS tests on physical devices
  via `xcrun devicectl`
- **On Android**: native C++ debugging requires `lldb-server` in the app's data
  directory (`/data/data/<package>/`), run via `run-as`, and configure all ART
  signals before attaching
- **Coverage**: `llvm-cov` with `LLVM_PROFILE_FILE` per test target, merge with
  `llvm-profdata`, exclude `_deps/` and test frameworks

---

## Common Mistakes

1. **Using `useState` for per-frame rendering data** — causes full React
   reconciliation per frame. Use refs and imperative drawing instead.
2. **Building SVG path strings to parse with `Skia.Path.MakeFromSVGString`** —
   causes float→string→parse overhead. Use `path.moveTo()` / `path.lineTo()` directly.
3. **Assuming `setPreferredIOBufferDuration` is honored** — always read back the
   granted value.
4. **Not calling `startBluetoothSco()` before starting audio on Android** — the
   SCO link needs explicit setup, and there's a timing race if you start audio
   before the `ACTION_SCO_AUDIO_STATE_UPDATED` broadcast confirms connection.
5. **Forgetting `--break-system-packages`** when pip-installing on Asahi Linux.
6. **Suggesting JUCE** when the project uses miniaudio/Oboe/CoreAudio directly —
   they're different ecosystems with different tradeoffs.
