#pragma once

#if defined(THL_PLATFORM_ANDROID)

#include <tanh/audio-io/AudioDeviceInfo.h>
#include <vector>

namespace thl {

/// Must be called once (e.g. from JNI_OnLoad) before enumeration works.
void set_android_java_vm(void* java_vm);

/// Configure Android audio routing for media (A2DP) at startup.
/// Ensures SCO/HFP is disabled and AudioManager mode is restored to normal.
void configure_android_bluetooth_session();

/// Start or stop the Bluetooth SCO link via AudioManager.
/// Must be called before enumerating devices so that SCO devices
/// are included/excluded from the list.
bool set_android_bluetooth_sco(bool enable);

/// Whether SCO is currently enabled.
bool is_android_bluetooth_sco_enabled();

/// Enumerate audio devices via Android AudioManager JNI.
/// Falls back to an empty list if the JavaVM has not been set.
std::vector<AudioDeviceInfo> enumerate_android_audio_devices(DeviceType type);

/// Returns the Android API level (e.g. 28 for Android 9, 29 for Android 10).
int get_android_api_level();

/// Returns true if a classic Bluetooth device (SCO / A2DP) is connected.
/// BLE Audio devices are excluded — they don't need profile switching.
bool is_android_classic_bluetooth_connected();

/// Query the name of the currently active output audio device via
/// AudioManager.getDevices(GET_DEVICES_OUTPUTS).
/// If aaudio_device_id > 0, looks up by Android device ID; otherwise returns
/// the first supported connected output device.
std::string get_android_active_output_device_name(int32_t aaudio_device_id = 0);

/// Query the name of the currently active input audio device via
/// AudioManager.getDevices(GET_DEVICES_INPUTS).
/// If aaudio_device_id > 0, looks up by Android device ID; otherwise returns
/// the first supported connected input device.
std::string get_android_active_input_device_name(int32_t aaudio_device_id = 0);

}  // namespace thl

#endif  // THL_PLATFORM_ANDROID
