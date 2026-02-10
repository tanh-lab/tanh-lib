#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace thl {

/**
 * @enum DeviceType
 * @brief Identifies the type of audio device.
 */
enum class DeviceType { Playback, Capture, Duplex };

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
     * @brief Type of the audio device (input, output, or duplex).
     *
     * Indicates whether this device is capable of audio capture (input),
     * playback (output), or both (duplex).
     */
    DeviceType deviceType = DeviceType::Playback;

    /**
     * @brief List of sample rates supported by the device.
     *
     * Contains the discrete sample rates that this device can operate at.
     * Common values include 44100, 48000, 88200, 96000, etc.
     */
    std::vector<uint32_t> sampleRates;

    /**
     * @brief Gets a pointer to the device ID for internal use.
     *
     * @return Pointer to the internal device ID storage.
     *
     * @note This is primarily used internally when initialising devices.
     */
    const void* deviceIDPtr() const { return m_deviceIdStorage.data(); }
    void* deviceIdStoragePtr() { return m_deviceIdStorage.data(); }

    static constexpr size_t kDeviceIdStorageSize = 256;

private:
    friend class AudioDeviceManager;

    std::array<char, kDeviceIdStorageSize> m_deviceIdStorage{};
};

}  // namespace thl
