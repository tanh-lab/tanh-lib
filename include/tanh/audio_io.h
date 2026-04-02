#pragma once

#include "audio-io/AudioDeviceManager.h"
#include "audio-io/AudioIODeviceCallback.h"
#include "audio-io/AudioDeviceInfo.h"

#if defined(THL_PLATFORM_IOS)
#include "audio-io/iOSAudioDevices.h"
#endif

#if defined(THL_PLATFORM_ANDROID)
#include "audio-io/AndroidAudioDevices.h"
#endif