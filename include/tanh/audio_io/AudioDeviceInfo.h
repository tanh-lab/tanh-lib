#pragma once
#include "miniaudio.h"
#include <string>
#include <vector>

namespace thl {

/**
 * @struct AudioDeviceInfo
 * @brief Contains information about an audio device available on the system.
 *
 * AudioDeviceInfo encapsulates the essential properties of an audio device,
 * including its display name, unique identifier, type (input/output), and
 * supported sample rates. This structure is primarily used when enumerating
 * available devices and selecting a device for initialisation.
 *
 * @see AudioDeviceManager::enumerateInputDevices()
 * @see AudioDeviceManager::enumerateOutputDevices()
 * @see AudioDeviceManager::initialise()
 */
struct AudioDeviceInfo {
    /**
     * @brief Human-readable name of the audio device.
     *
     * This is the display name provided by the operating system or driver,
     * suitable for showing to users in device selection interfaces.
     */
    std::string name;

    /**
     * @brief Unique identifier for the audio device.
     *
     * This identifier is used internally by miniaudio to reference the specific
     * device. The identifier format is platform-specific.
     */
    ma_device_id deviceID;

    /**
     * @brief Type of the audio device (input, output, or duplex).
     *
     * Indicates whether this device is capable of audio capture (input),
     * playback (output), or both (duplex).
     *
     * @see ma_device_type for possible values
     */
    ma_device_type deviceType;

    /**
     * @brief List of sample rates supported by the device.
     *
     * Contains the discrete sample rates that this device can operate at.
     * Common values include 44100, 48000, 88200, 96000, etc.
     */
    std::vector<ma_uint32> sampleRates;

    /**
     * @brief Gets a pointer to the device ID for use with miniaudio functions.
     *
     * @return Pointer to the internal device ID structure.
     *
     * @note This is primarily used internally when initialising devices.
     */
    const ma_device_id* deviceIDPtr() const { return &deviceID; }
};

}  // namespace thl
