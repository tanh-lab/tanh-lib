#pragma once

#if defined(THL_PLATFORM_ANDROID)

#include <tanh/audio_io/AudioDeviceInfo.h>
#include <vector>

namespace thl {

/// Must be called once (e.g. from JNI_OnLoad) before enumeration works.
void setAndroidJavaVM(void* javaVM);

/// Configure Android audio routing for media (A2DP) at startup.
/// Ensures SCO/HFP is disabled and AudioManager mode is restored to normal.
void configureAndroidBluetoothSession();

/// Start or stop the Bluetooth SCO link via AudioManager.
/// Must be called before enumerating devices so that SCO devices
/// are included/excluded from the list.
bool setAndroidBluetoothSco(bool enable);

/// Whether SCO is currently enabled.
bool isAndroidBluetoothScoEnabled();

/// Enumerate audio devices via Android AudioManager JNI.
/// Falls back to an empty list if the JavaVM has not been set.
std::vector<AudioDeviceInfo> enumerateAndroidAudioDevices(DeviceType type);

/// Returns the Android API level (e.g. 28 for Android 9, 29 for Android 10).
int getAndroidApiLevel();

/// Returns true if a classic Bluetooth device (SCO / A2DP) is connected.
/// BLE Audio devices are excluded — they don't need profile switching.
bool isAndroidClassicBluetoothConnected();

/// Returns the native sample rate for the Bluetooth SCO capture device
/// as reported by AudioDeviceInfo.getSampleRates().  Returns the highest
/// reported rate, or 16000 (mSBC default) if no rates are advertised.
/// Returns 0 if no SCO input device is found or JNI is unavailable.
uint32_t getAndroidScoSampleRate();

/// Query the name of the currently active output audio device via
/// AudioManager.getDevices(GET_DEVICES_OUTPUTS).
/// If aaudioDeviceId > 0, looks up by Android device ID; otherwise returns
/// the first supported connected output device.
std::string getAndroidActiveOutputDeviceName(int32_t aaudioDeviceId = 0);

/// Query the name of the currently active input audio device via
/// AudioManager.getDevices(GET_DEVICES_INPUTS).
/// If aaudioDeviceId > 0, looks up by Android device ID; otherwise returns
/// the first supported connected input device.
std::string getAndroidActiveInputDeviceName(int32_t aaudioDeviceId = 0);

}  // namespace thl

#endif  // THL_PLATFORM_ANDROID
