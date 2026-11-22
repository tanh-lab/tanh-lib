#pragma once

#include <tanh/core/AbiNamespace.h>
#include <tanh/core/Exports.h>
#include <tanh/midi/DeviceInfo.h>
#include <tanh/midi/InputQueue.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace thl::inline THL_ABI_NAMESPACE::midi {

/// Settings of a DeviceManager, fixed at construction.
struct DeviceManagerConfig {
    /// The protocol to receive. Midi2 delivers MIDI 2.0 channel voice packets (type 4)
    /// wherever the platform or the device provides them; Midi1 asks for MIDI 1.0 (type 2)
    /// throughout. Where the platform does not convert (Android), packets arrive in the
    /// device's own protocol, so a consumer handles both types either way — or converts
    /// with convert_to_midi1().
    Protocol m_protocol = Protocol::Midi2;
    /// Capacity of the input queue, in packets.
    std::size_t m_queue_capacity = InputQueue::k_default_capacity;
    /// Open every input that is present or appears later.
    bool m_auto_open = true;
    /// Deliver System Exclusive (types 3 and 5). Off by default: a dump is thousands of
    /// packets and would crowd the performance data out of the queue.
    bool m_receive_sysex = false;
    /// Name under which this app appears to the system's MIDI service. On macOS and iOS
    /// the managers of a process share one MIDI client, named by the first of them; each
    /// manager's own name is that of its input port.
    std::string m_client_name = "tanh";
};

/// MIDI inputs of the device: lists them, opens them, follows hot-plugging, and delivers
/// what they send as timestamped Universal MIDI Packets into one InputQueue.
///
/// Backed by CoreMIDI on macOS and iOS and by android.media.midi on Android (see
/// AndroidMidi.h for what the app has to provide). On other platforms supported() is
/// false, the list stays empty and the queue only carries what the application pushes
/// itself.
///
/// CoreMIDI serves a whole process through the run loop of the thread that makes the
/// process's first CoreMIDI call (any call, not only creating a client); without that run
/// loop running, devices that come and go are never seen. If the application has not
/// used CoreMIDI when the first manager is constructed, the backend starts a thread that
/// takes that role and runs until the process ends, so the manager works from any thread.
/// An application that wants CoreMIDI on its main run loop makes a CoreMIDI call there
/// first. Either way the backend checks once that notifications arrive and logs a warning
/// (group thl.midi.device_manager) when they do not. All managers of a process share one
/// MIDI client, which is kept until the process ends; a manager owns its input port and
/// its virtual input.
///
/// All methods are for control threads and may block briefly; none is real-time safe. The
/// audio thread only touches input_queue().
class TANH_API DeviceManager {
public:
    /// Called when an input appeared, went away, changed its name or (Android) finished
    /// opening. Runs on a thread of the backend: do not block it, and do not destroy the
    /// manager from inside it.
    using DeviceChangeCallback = std::function<void()>;

    explicit DeviceManager(const DeviceManagerConfig& config = {});
    ~DeviceManager();

    DeviceManager(const DeviceManager&) = delete;
    DeviceManager& operator=(const DeviceManager&) = delete;
    DeviceManager(DeviceManager&&) = delete;
    DeviceManager& operator=(DeviceManager&&) = delete;

    /// Whether this build has a device backend for the platform it runs on.
    [[nodiscard]] static bool supported();

    /// Whether the backend is connected to the system's MIDI service. False when
    /// unsupported, or when the service refused the connection.
    [[nodiscard]] bool running() const;

    /// The inputs currently present, including this manager's own virtual input.
    [[nodiscard]] std::vector<DeviceInfo> input_devices() const;

    /// Starts delivering the input with this DeviceInfo::m_id. True when it is open
    /// afterwards — or, on Android, where the system opens a device asynchronously, when
    /// it is being opened: the device change callback then tells when DeviceInfo::m_open
    /// has changed.
    bool open_input(std::uint32_t id);

    /// Stops delivering it. True when it was present.
    bool close_input(std::uint32_t id);

    /// Whether inputs that appear from now on are opened automatically. Turning it on also
    /// opens the ones already present.
    void set_auto_open(bool auto_open);

    /// Sets (or, with an empty function, clears) the device change callback.
    void set_device_change_callback(DeviceChangeCallback callback);

    /// Creates a virtual input with this name, to which other apps on the device can send.
    /// It is listed among the inputs, open, with Transport::Virtual; close_input() stops
    /// its delivery and open_input() resumes it, while the port stays. Returns false where
    /// the platform has no virtual ports or refuses (iOS requires the `audio` background
    /// mode), and when one already exists.
    bool create_virtual_input(const std::string& name);

    /// The queue every open input delivers into. Lives as long as the manager.
    [[nodiscard]] InputQueue& input_queue() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace thl::inline THL_ABI_NAMESPACE::midi
