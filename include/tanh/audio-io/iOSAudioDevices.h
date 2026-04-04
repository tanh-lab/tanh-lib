#pragma once

#if defined(THL_PLATFORM_IOS)

#include <tanh/audio-io/AudioDeviceInfo.h>
#include <string>
#include <vector>

// Forward-declare so we don't pull in the full AudioDeviceManager header.
namespace thl {
enum class BluetoothProfile;
}

namespace thl {

/// Describes an iOS audio route (port).
struct AudioRouteInfo {
    std::string m_name;       ///< Human-readable port name (e.g. "iPhone Microphone").
    std::string m_port_type;  ///< Port type identifier (e.g. "MicrophoneBuiltIn", "BluetoothHFP").
    std::string m_uid;        ///< Unique identifier used for route selection.
};

/// Configure the initial AVAudioSession category for playback + record.
/// Called once during AudioDeviceManager construction.
void configure_ios_audio_session();

/// Returns true if any Bluetooth route (A2DP, HFP, or LE) is currently active.
bool is_ios_bluetooth_route_active();

/// Returns true if a classic Bluetooth device (A2DP / HFP) is connected.
/// BLE Audio devices are excluded — they don't need profile switching.
bool is_ios_classic_bluetooth_connected();

/// Reconfigure AVAudioSession category options for the given Bluetooth profile.
/// Returns true on success.
bool set_ios_bluetooth_profile(BluetoothProfile profile, const char** out_profile_name);

/// Returns all available audio input routes via AVAudioSession.
std::vector<AudioRouteInfo> get_ios_available_input_routes();

/// Selects the preferred audio input route by matching the given UID.
bool set_ios_preferred_input_route(const AudioRouteInfo& route);

/// Forces audio output to the built-in speaker (or restores default routing).
bool override_ios_output_to_speaker(bool to_speaker);

/// Returns the name of the currently active input route.
std::string get_ios_current_input_route_name();

/// Returns the name of the currently active output route.
std::string get_ios_current_output_route_name();

}  // namespace thl

#endif  // THL_PLATFORM_IOS
