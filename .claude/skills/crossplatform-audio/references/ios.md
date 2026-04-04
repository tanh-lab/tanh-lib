# iOS Audio Reference

## Table of Contents
1. [CoreAudio and AVAudioSession](#coreaudio-and-avaudiosession)
2. [Buffer Size Constraints](#buffer-size-constraints)
3. [Bluetooth Audio on iOS](#bluetooth-audio-on-ios)
4. [AUv3 Extension Architecture](#auv3-extension-architecture)
5. [CMake iOS Builds](#cmake-ios-builds)
6. [Debugging and Testing](#debugging-and-testing)

---

## CoreAudio and AVAudioSession

### AVAudioSession categories

| Category | Use Case | Mixes with others | Bluetooth |
|----------|----------|-------------------|-----------|
| `playback` | Music, games | No (ducks others) | A2DP only |
| `playAndRecord` | VoIP, instruments | Configurable | A2DP or HFP |
| `ambient` | Background audio | Yes | A2DP only |
| `multiRoute` | Multi-output | Yes | Limited |

### Category options for Bluetooth:

```objc
// A2DP only (high quality, no mic)
[session setCategory:AVAudioSessionCategoryPlayback
         withOptions:AVAudioSessionCategoryOptionAllowBluetoothA2DP
               error:&error];

// HFP (low quality, with mic)
[session setCategory:AVAudioSessionCategoryPlayAndRecord
         withOptions:AVAudioSessionCategoryOptionAllowBluetooth
               error:&error];
```

`allowBluetooth` → HFP/SCO (16kHz, mic enabled)
`allowBluetoothA2DP` → A2DP (44.1–48kHz, no mic)

### Session activation

Always check return values:
```objc
NSError *error = nil;
BOOL success = [session setActive:YES error:&error];
if (!success) {
    // Handle — this is a real failure, not just a warning
}
```

### Route change notifications

```objc
[[NSNotificationCenter defaultCenter]
    addObserver:self
       selector:@selector(handleRouteChange:)
           name:AVAudioSessionRouteChangeNotification
         object:nil];
```

Route changes require re-reading the sample rate, buffer duration, and channel
count — they may all change.

---

## Buffer Size Constraints

### setPreferredIOBufferDuration

- **Maximum**: ~0.093 seconds (documented ceiling)
- Requesting more (e.g., `2048/16000 = 0.128s`) → iOS silently clamps or ignores
- **Always read back the granted value**:
  ```objc
  NSTimeInterval granted = session.IOBufferDuration;
  ```

### Bluetooth SCO buffer constraints

SCO at 16kHz transmits fixed ~120-frame packets every 7.5ms. Large buffer
requests are structurally incompatible with the SCO protocol. iOS outputs
**silence** rather than corrupted audio when the SCO driver cannot accommodate
the requested buffering depth.

### MaximumFramesPerSlice

When setting buffer duration, also sync the Audio Unit property:
```objc
UInt32 maxFrames = grantedBufferSize;
AudioUnitSetProperty(audioUnit,
    kAudioUnitProperty_MaximumFramesPerSlice,
    kAudioUnitScope_Global, 0,
    &maxFrames, sizeof(maxFrames));
```

### Decoupling hardware and DSP buffer sizes

If your DSP core needs a larger block than the hardware can provide, use a ring
buffer between the callback and your processing:

```
CoreAudio callback (128 frames) → ring buffer → DSP core (512 frames)
```

Don't try to force the hardware to match your DSP block size.

---

## Bluetooth Audio on iOS

### A2DP vs HFP switching

- iOS negotiates the profile based on `AVAudioSession` category options
- Switching takes 1–3 seconds — don't do it in response to transient events
- When switching from A2DP to HFP, sample rate drops from 44.1–48kHz to 16kHz
- AirPods behave identically to generic Bluetooth headphones in this regard

### iOS 26: bluetoothHighQualityRecording

New `AVAudioSessionCategoryOption` that enables simultaneous high-quality input
and output for AirPods specifically. Not available for third-party Bluetooth
headphones.

### LE Audio / Auracast

As of mid-2025, Apple has not shipped LE Audio support in iOS despite hardware
Bluetooth 5.2+ support. Apple has explicitly stated no plans to support anything
beyond MFi for Bluetooth audio.

### Sample rate handling

Unlike Android, Apple's unified CoreAudio stack performs symmetric resampling in
both directions (input and output) and reports sample rates truthfully via
`routeChangeNotification`. No workarounds needed for the mismatch bug that
plagues Android.

---

## AUv3 Extension Architecture

### Key constraint

AUv3 extensions run as **separate OS processes**. React Native's bridge and JS
engine **cannot run** inside an AUv3 extension.

### Architecture:

```
Standalone App (React Native)     AUv3 Extension (no RN)
├── JS/TS UI                      ├── CHOC WebView UI (or SwiftUI)
├── TurboModule bridge            ├── AUAudioUnit subclass
└── AudioDSPCore (shared C++)     └── AudioDSPCore (same shared C++)
```

The shared C++ DSP core is compiled into both targets independently.

### AUAudioUnit implementation:

```objc
@interface MyAudioUnit : AUAudioUnit
@end

@implementation MyAudioUnit {
    std::unique_ptr<AudioDSPCore> _dspCore;
    AUAudioUnitBus *_outputBus;
}

- (AUInternalRenderBlock)internalRenderBlock {
    // Capture raw pointer — no Objective-C calls in render block
    auto* core = _dspCore.get();

    return ^AUAudioUnitStatus(
        AudioUnitRenderActionFlags *actionFlags,
        const AudioTimeStamp *timestamp,
        AVAudioFrameCount frameCount,
        NSInteger outputBusNumber,
        AudioBufferList *outputData,
        const AURenderEvent *realtimeEventListHead,
        AURenderPullInputBlock pullInputBlock)
    {
        // Process AURenderEvents for parameter changes
        // Call DSP core — must be real-time safe
        core->process(outputData, frameCount);
        return noErr;
    };
}
```

### AUParameterTree

Use `AUParameterTree` for host-visible parameters. Parameter changes arrive as
`AURenderEvent` in the render block — process them inline for sample-accurate
automation.

### Lock-free parameter sync

The render block captures a raw C++ pointer. Use atomics or a lock-free FIFO for
parameter changes from the UI thread to the render thread.

---

## CMake iOS Builds

### Toolchain:

```cmake
cmake_minimum_required(VERSION 3.20)
project(MyApp)

set(CMAKE_SYSTEM_NAME iOS)
set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0")
set(CMAKE_OSX_ARCHITECTURES "arm64")
set(CMAKE_CXX_STANDARD 17)
```

### Build and open in Xcode:

```bash
cmake -B build-ios -GXcode \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 \
  -DCMAKE_OSX_ARCHITECTURES=arm64

open build-ios/MyApp.xcodeproj
```

### Objective-C++ bridging

Use `.mm` extension files for bridging C++ to iOS APIs:
```objc
// Bridge.mm
#include "AudioDSPCore.hpp"
@implementation Bridge {
    std::unique_ptr<AudioDSPCore> engine;
}
```

ARC rules in `.mm` files: no manual `release`, no `[super dealloc]`, but C++
destructor cleanup in `dealloc` is fine.

---

## Debugging and Testing

### Physical device testing with CMake/CTest

Use `CMAKE_CROSSCOMPILING_EMULATOR` with a wrapper script around
`xcrun devicectl device process launch --console`:

```bash
#!/bin/bash
# Resolves .app bundle, extracts bundle ID, discovers device,
# installs, and launches with --console for stdout/stderr capture.
# Note: --console does NOT capture os_log/NSLog.
```

### miniaudio iOS-specific issues

- Silence fallback: miniaudio logs `WARNING: Outputting silence` when
  `inNumberFrames` mismatches `originalPeriodSizeInFrames`
- Interruption handling: miniaudio's CoreAudio backend has known rough edges
  around `AVAudioSessionInterruptionNotification`
- Bluetooth routing: miniaudio doesn't automatically handle A2DP↔HFP switching;
  you need to manage `AVAudioSession` category options yourself

### Hermes and JIT

Hermes (React Native's JS engine) compiles to native bytecode, not interpreted
JS. It is NOT subject to Apple's JIT restrictions because it uses AOT compilation.
This is important context when discussing React Native on iOS — don't suggest JIT
limitations apply.
