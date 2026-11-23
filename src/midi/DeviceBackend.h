#pragma once

#include <tanh/core/AbiNamespace.h>
#include <tanh/midi/DeviceInfo.h>
#include <tanh/midi/InputQueue.h>
#include <tanh/midi/Ump.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace thl::inline THL_ABI_NAMESPACE::midi {

/// What a backend gets from the DeviceManager that owns it.
struct BackendContext {
    /// Shared, not borrowed: a platform receive callback that is still running while the
    /// manager goes away keeps the queue alive through its own reference.
    std::shared_ptr<InputQueue> m_queue;
    Protocol m_protocol = Protocol::Midi2;
    bool m_auto_open = true;
    bool m_receive_sysex = false;
    std::string m_client_name;
    /// Called by the backend, from one of its threads, when the device list changed.
    std::function<void()> m_on_change;
};

/// The platform side of DeviceManager. One implementation per platform, selected at build
/// time (DeviceBackend_<platform>); the methods mirror DeviceManager's.
class DeviceBackend {
public:
    DeviceBackend() = default;
    virtual ~DeviceBackend() = default;

    DeviceBackend(const DeviceBackend&) = delete;
    DeviceBackend& operator=(const DeviceBackend&) = delete;
    DeviceBackend(DeviceBackend&&) = delete;
    DeviceBackend& operator=(DeviceBackend&&) = delete;

    [[nodiscard]] virtual bool running() const = 0;
    [[nodiscard]] virtual std::vector<DeviceInfo> input_devices() const = 0;
    virtual bool open_input(std::uint32_t id) = 0;
    virtual bool close_input(std::uint32_t id) = 0;
    virtual void set_auto_open(bool auto_open) = 0;
    virtual bool create_virtual_input(const std::string& name) = 0;
};

/// Whether this build has a backend for the platform.
[[nodiscard]] bool device_backend_supported();

/// The platform's backend, already started; never null.
[[nodiscard]] std::unique_ptr<DeviceBackend> make_device_backend(BackendContext context);

/// Which packets a backend puts into the queue. Channel voice and system messages always;
/// System Exclusive (types 3 and 5) on request. Utility (jitter-reduction clock, NOOP),
/// flex data, UMP stream and reserved types never: they are transport housekeeping, not
/// performance data.
[[nodiscard]] constexpr bool accepts_packet(const Ump& ump, bool receive_sysex) noexcept {
    switch (ump.message_type()) {
        case MessageType::System:
        case MessageType::Midi1ChannelVoice:
        case MessageType::Midi2ChannelVoice: return true;
        case MessageType::SysEx7:
        case MessageType::Data128: return receive_sysex;
        case MessageType::Utility:
        case MessageType::FlexData:
        case MessageType::Stream: return false;
    }
    return false;
}

}  // namespace thl::inline THL_ABI_NAMESPACE::midi
