#pragma once

#if defined(THL_PLATFORM_ANDROID)

#include <tanh/audio_io/AudioDeviceInfo.h>
#include <vector>

namespace thl {

/// Must be called once (e.g. from JNI_OnLoad) before enumeration works.
void setAndroidJavaVM(void* javaVM);

/// Start or stop the Bluetooth SCO link via AudioManager.
/// Must be called before enumerating devices so that SCO devices
/// are included/excluded from the list.
bool setAndroidBluetoothSco(bool enable);

/// Whether SCO is currently enabled.
bool isAndroidBluetoothScoEnabled();

/// Enumerate audio devices via Android AudioManager JNI.
/// Falls back to an empty list if the JavaVM has not been set.
std::vector<AudioDeviceInfo> enumerateAndroidAudioDevices(DeviceType type);

}  // namespace thl

#endif  // THL_PLATFORM_ANDROID
