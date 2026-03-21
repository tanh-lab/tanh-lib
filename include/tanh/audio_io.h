#pragma once

#include "audio_io/AudioDeviceManager.h"
#include "audio_io/AudioIODeviceCallback.h"
#include "audio_io/AudioDeviceInfo.h"

#if defined(THL_PLATFORM_IOS)
#include "audio_io/iOSAudioDevices.h"
#endif

#if defined(THL_PLATFORM_ANDROID)
#include "audio_io/AndroidAudioDevices.h"
#endif