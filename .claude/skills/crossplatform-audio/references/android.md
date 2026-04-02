# Android Audio Reference

## Table of Contents
1. [AAudio Architecture](#aaudio-architecture)
2. [miniaudio Android Backend](#miniaudio-android-backend)
3. [JNI Device Enumeration](#jni-device-enumeration)
4. [Bluetooth SCO on Android](#bluetooth-sco-on-android)
5. [Samsung and OEM Quirks](#samsung-and-oem-quirks)
6. [Debugging Android Audio](#debugging-android-audio)

---

## AAudio Architecture

AAudio operates in two modes:

- **MMAP mode** (FAST path): Direct shared memory between app and HAL. Lowest
  latency. Not available on all devices or for all stream configurations.
- **Legacy mode**: Goes through AudioFlinger. Higher latency but more compatible.

### framesPerBurst

This is the atomic DMA transfer size — the hardware's minimum latency floor.
**You cannot reduce it from software.** It varies by device:

| Device Family | Typical framesPerBurst | Latency at 48kHz |
|---------------|----------------------|-------------------|
| Google Pixel  | 48–192               | 1–4 ms            |
| Samsung Galaxy| 960–1536             | 20–32 ms          |
| OnePlus       | 192–480              | 4–10 ms           |

Samsung's HAL statically configures large burst sizes. This is a hard floor that
no library (Oboe, miniaudio, or otherwise) can circumvent.

### Buffer capacity vs burst size

- `framesPerBurst`: callback/DMA chunk size (fixed by HAL)
- `bufferCapacityInFrames`: total ring buffer size (adjustable via
  `AAudioStreamBuilder_setBufferCapacityInFrames` on API 26+)
- Practical minimum latency = `framesPerBurst × 2` (double buffering)

### FAST path requirements

AAudio only uses the FAST/MMAP path when:
- Performance mode is `AAUDIO_PERFORMANCE_MODE_LOW_LATENCY`
- Sample rate matches the device native rate (usually 48000)
- Channel count is mono or stereo
- Format is `FLOAT` or `INT16`
- Sharing mode is `EXCLUSIVE` (not `SHARED`)
- The device supports MMAP

Check with: `adb shell dumpsys audio | grep -A5 "MMAP"`

---

## miniaudio Android Backend

### Key configuration flags

```c
ma_device_config config = ma_device_config_init(ma_device_type_playback);

// AAudio-specific sub-config
config.aaudio.usage = MA_AAUDIO_USAGE_MEDIA;  // or VOICE_COMMUNICATION for HFP
config.aaudio.allowSetBufferCapacity = MA_TRUE;  // DEFAULT IS FALSE
config.aaudio.noAutoStartAfterReroute = MA_FALSE;

// Fixed callback size (enables intermediary buffer)
config.noFixedSizedCallback = MA_FALSE;  // DEFAULT
config.periodSizeInFrames = 256;  // your desired DSP block size
config.periods = 2;
```

### Critical: `allowSetBufferCapacity`

This is **disabled by default**. Without it, the `periods` multiplier has no
effect on AAudio's actual hardware buffer. AAudio uses a driver-determined default.
Enable it explicitly if you need buffer capacity control.

### Intermediary buffer

When `noFixedSizedCallback = MA_FALSE` (default), miniaudio inserts a buffer
between AAudio's variable-burst callbacks and your fixed-size callback. This
repackages AAudio's variable burst sizes into consistent `periodSizeInFrames`
chunks. The intermediary buffer size behavior should be verified by reading
`ma_device_handle_backend_data_callback()` in the miniaudio source.

### Usage hints and routing

```
MA_AAUDIO_USAGE_MEDIA (1)                → A2DP Bluetooth, speaker, wired
MA_AAUDIO_USAGE_VOICE_COMMUNICATION (2)  → HFP/SCO Bluetooth, earpiece
```

Setting `VOICE_COMMUNICATION` triggers the Android audio policy to negotiate SCO
if Bluetooth headphones are connected.

---

## JNI Device Enumeration

### Bootstrap pattern (no Activity reference needed):

```cpp
// Get Application context via ActivityThread
jclass activityThread = env->FindClass("android/app/ActivityThread");
jmethodID currentApp = env->GetStaticMethodID(activityThread,
    "currentApplication", "()Landroid/app/Application;");
jobject context = env->CallStaticObjectMethod(activityThread, currentApp);

// Get AudioManager
jmethodID getSystemService = env->GetMethodID(
    env->GetObjectClass(context), "getSystemService",
    "(Ljava/lang/String;)Ljava/lang/Object;");
jstring audioServiceName = env->NewStringUTF("audio");
jobject audioManager = env->CallObjectMethod(context, getSystemService, audioServiceName);

// Enumerate devices
jmethodID getDevices = env->GetMethodID(
    env->GetObjectClass(audioManager), "getDevices", "(I)[Landroid/media/AudioDeviceInfo;");
jobjectArray devices = (jobjectArray)env->CallObjectMethod(audioManager, getDevices, 3);
```

### ScopedJNIEnv RAII pattern

For calling JNI from non-Java threads (like the audio device notification callback):

```cpp
struct ScopedJNIEnv {
    JNIEnv* env;
    bool didAttach;
    ScopedJNIEnv(JavaVM* vm) : didAttach(false) {
        if (vm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
            vm->AttachCurrentThread(&env, nullptr);
            didAttach = true;
        }
    }
    ~ScopedJNIEnv() {
        if (didAttach) {
            JavaVM* vm; env->GetJavaVM(&vm);
            vm->DetachCurrentThread();
        }
    }
};
```

### Device ID bridging

`AudioDeviceInfo.getId()` returns an integer that maps to `ma_device_id.aaudio`
in miniaudio. Use this to target a specific device when creating a stream.

---

## Bluetooth SCO on Android

### Explicit SCO setup (Kotlin):

```kotlin
fun setBluetoothProfile(useSco: Boolean) {
    val audioManager = getSystemService(Context.AUDIO_SERVICE) as AudioManager
    if (useSco) {
        val receiver = object : BroadcastReceiver() {
            override fun onReceive(context: Context, intent: Intent) {
                val state = intent.getIntExtra(AudioManager.EXTRA_SCO_AUDIO_STATE, -1)
                if (state == AudioManager.SCO_AUDIO_STATE_CONNECTED) {
                    // NOW safe to start audio with VOICE_COMMUNICATION usage
                    unregisterReceiver(this)
                }
            }
        }
        registerReceiver(receiver,
            IntentFilter(AudioManager.ACTION_SCO_AUDIO_STATE_UPDATED))
        audioManager.startBluetoothSco()
    } else {
        audioManager.stopBluetoothSco()
    }
}
```

### Timing race

**Critical**: `startBluetoothSco()` is async. If you call `startAudio()` before
the `SCO_AUDIO_STATE_CONNECTED` broadcast arrives, the stream may open on the
wrong output or fail silently.

### Sample rate mismatch bug

Android's HAL correctly resamples the **output** path (48kHz → 16kHz for SCO)
but often skips resampling on the **input** path while still reporting 48kHz to
apps. Recordings from SCO mic play back at wrong speed. Workarounds:
1. Open `AudioRecord` at 16kHz directly
2. Resample in-app
3. Use `startBluetoothSco()` with explicit rate configuration

---

## Samsung and OEM Quirks

Oboe's `QuirksManager` handles many Samsung-specific issues:

- **MMAP corruption**: Some Samsung devices corrupt audio with MMAP. QuirksManager
  forces legacy mode.
- **Minimum buffer enforcement**: Samsung enforces minimum buffer sizes larger than
  spec requires.
- **Stuck stream states**: Streams can enter a stuck state requiring full teardown
  and recreation.

**None of these workarounds reduce the fundamental burst size.** If Samsung's HAL
reports `framesPerBurst = 1536`, that's a 32ms floor at 48kHz.

Check device API level: `adb shell getprop ro.build.version.sdk`

---

## Debugging Android Audio

### Native C++ debugging on Android

1. Find `lldb-server` in NDK: `find $ANDROID_NDK -name "lldb-server" | grep arm64`
2. Push to device:
   ```bash
   adb push lldb-server /data/local/tmp/
   adb shell run-as com.your.package cp /data/local/tmp/lldb-server .
   adb shell run-as com.your.package chmod +x lldb-server
   ```
3. Start server:
   ```bash
   adb shell run-as com.your.package ./lldb-server platform \
     --listen "*:9123" --server
   ```
4. **Before attaching**: Configure ART signal handling to avoid freeze:
   ```
   process handle SIGSEGV -n true -p true -s false
   process handle SIGILL -n true -p true -s false
   process handle SIGTRAP -n true -p true -s false
   process handle SIGBUS -n true -p true -s false
   ```
   These must be set **before** `process attach`, not after.

5. Known issue: LLDB may freeze on attach due to symbol loading blocking
   `--continue`. The `ptrace` stop (`State: t`) with no signal indicates LLDB
   is loading symbols.

### Android Studio debugging

- Open the `android/` folder directly: `open -a "Android Studio" ~/project/android`
  (inherits shell PATH for `node`)
- Use "Native Only" debug configuration for C++ debugging
- Android Studio manages its own `lldb-server` — conflicts with manually started
  instances

### Useful adb commands

```bash
adb shell dumpsys audio                    # Full audio state
adb shell dumpsys audio | grep -i mmap     # MMAP support
adb shell getprop ro.build.version.sdk     # API level
adb shell getprop ro.product.manufacturer  # OEM (for quirk identification)
```

### AudioTrack::requestExitAndWait() deadlock

Known issue on API 28 and below: destroying audio streams from the audio callback
thread causes a deadlock in `AudioTrack::requestExitAndWait()`. Always destroy
streams from a non-audio thread.
