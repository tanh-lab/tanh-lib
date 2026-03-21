#pragma once

#if defined(THL_PLATFORM_IOS)

#include <tanh/audio_io/AudioDeviceInfo.h>
#include <string>
#include <vector>

// Forward-declare so we don't pull in the full AudioDeviceManager header.
namespace thl { enum class BluetoothProfile; }

namespace thl {

/// Describes an iOS audio route (port).
struct AudioRouteInfo {
    std::string name;      ///< Human-readable port name (e.g. "iPhone Microphone").
    std::string portType;  ///< Port type identifier (e.g. "MicrophoneBuiltIn", "BluetoothHFP").
    std::string uid;       ///< Unique identifier used for route selection.
};

/// Configure the initial AVAudioSession category for playback + record.
/// Called once during AudioDeviceManager construction.
void configureIOSAudioSession();

/// Returns true if any Bluetooth route (A2DP, HFP, or LE) is currently active.
bool isIOSBluetoothRouteActive();

/// Reconfigure AVAudioSession category options for the given Bluetooth profile.
/// Returns true on success.
bool setIOSBluetoothProfile(BluetoothProfile profile, const char** outProfileName);

/// Returns all available audio input routes via AVAudioSession.
std::vector<AudioRouteInfo> getIOSAvailableInputRoutes();

/// Selects the preferred audio input route by matching the given UID.
bool setIOSPreferredInputRoute(const AudioRouteInfo& route);

/// Forces audio output to the built-in speaker (or restores default routing).
bool overrideIOSOutputToSpeaker(bool toSpeaker);

/// Returns the name of the currently active input route.
std::string getIOSCurrentInputRouteName();

/// Returns the name of the currently active output route.
std::string getIOSCurrentOutputRouteName();

}  // namespace thl

#endif  // THL_PLATFORM_IOS
