# Audio IO iOS Test App

This is a minimal iOS application for testing the `tanh` audio_io library on iOS devices.

## Features

- Simple UI with Start/Stop buttons
- Device enumeration logging
- Audio callback verification
- Real-time logging of audio processing

## Building

### Option 1: Command Line (Recommended for quick builds)

From the project root directory:

```bash
just build-ios
```

This builds only the `audio_io` app target, avoiding issues with miniaudio node libraries.

### Option 2: Xcode

```bash
just open-ios
```

**Important**: In Xcode, you must select the `audio_io` scheme from the scheme selector (next to the Run/Stop buttons) before building. Do NOT build the `ALL_BUILD` scheme as it will fail due to miniaudio node library linking issues.

1. Open the project with `just open-ios`
2. In Xcode, click the scheme selector (shows current scheme name)
3. Select `audio_io` from the list
4. Select your target device (simulator or physical device)
5. Click Run (⌘R)

## Running

### From Xcode
1. Select the `audio_io` scheme
2. Choose a simulator or connect a physical iOS device
3. Click Run (⌘R) or Build (⌘B)
4. The app will launch on your selected device

### What to expect


1. Open the generated Xcode project
2. Select a real iOS device or simulator
3. Build and run the app
4. Tap "Start Audio" to begin audio processing
5. Watch the logs to verify callbacks are being executed
6. Tap "Stop Audio" to halt processing

## What It Tests

- **AudioDeviceManager initialization**: Verifies the audio context can be created
- **Device enumeration**: Lists all available output devices
- **Device configuration**: Initializes with 48kHz, 512 frames, stereo output
- **Callback registration**: Adds a simple audio callback that outputs silence
- **Start/Stop lifecycle**: Tests the full audio lifecycle
- **Real-time callbacks**: Logs every 100 callbacks to verify continuous processing

## Debugging Tips

- Check the log view in the app for initialization messages
- Callback count should increment if audio is processing correctly
- If audio fails to start, check device permissions in iOS Settings
- For detailed debugging, use Xcode's console output

## Implementation Notes

- Uses `miniaudio` backend via `AudioDeviceManager`
- Callback outputs silence (zeros) for simplicity
- All UI operations are on main thread, audio processing on dedicated thread
- Proper cleanup in `dealloc`
