#pragma once

#include <tanh/core/AbiNamespace.h>

#include <cstdint>
#include <string>

namespace thl::inline THL_ABI_NAMESPACE::midi {

/// How a MIDI device is attached. Best effort: Unknown where the platform does not say.
enum class Transport : std::uint8_t {
    Unknown,
    Usb,
    Bluetooth,
    Network,
    Virtual,  ///< Another app's virtual port, or this app's own virtual input.
};

/// The MIDI protocol a device speaks, or that a consumer asks to receive.
enum class Protocol : std::uint8_t {
    Midi1,  ///< MIDI 1.0: packets of type 2 (and 1, 3).
    Midi2,  ///< MIDI 2.0: packets of type 4 (and 1, 3).
};

/// One MIDI input as DeviceManager::input_devices() lists it.
struct DeviceInfo {
    /// Identifies the input while it is connected; pass it to open_input() / close_input().
    /// Not stable across reconnects or app runs — use m_persistent_key to remember a device.
    std::uint32_t m_id = 0;
    std::string m_name;          ///< Name to show to the user.
    std::string m_manufacturer;  ///< May be empty.
    Transport m_transport = Transport::Unknown;
    Protocol m_protocol = Protocol::Midi1;  ///< What the device itself speaks.
    /// Stays the same when the device is unplugged and plugged in again, as far as the
    /// platform can tell: the key under which to store per-device settings.
    std::string m_persistent_key;
    /// TimedUmp::m_source of the packets this input delivers. Valid while it is listed.
    std::uint16_t m_source = 0;
    bool m_open = false;  ///< Whether its packets currently arrive in the queue.
};

}  // namespace thl::inline THL_ABI_NAMESPACE::midi
