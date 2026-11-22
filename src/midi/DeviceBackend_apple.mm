#include "DeviceBackend.h"

#include <tanh/core/AbiNamespace.h>
#include <tanh/core/Clock.h>
#include <tanh/core/Logger.h>
#include <tanh/midi/DeviceInfo.h>
#include <tanh/midi/InputQueue.h>
#include <tanh/midi/Ump.h>

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#include <dispatch/dispatch.h>
#include <mach/mach_time.h>
#include <pthread.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// CoreMIDI backend for macOS and iOS. Compiled with ARC (CMakeLists.txt), which owns the
// dispatch queue and the blocks; CoreFoundation objects are released by hand.
//
// Everything a CoreMIDI or dispatch block touches lives in a shared State. The blocks hold
// it (or the queue) through shared / weak pointers, never through the backend object, so a
// callback that is still running while the DeviceManager is destroyed finds valid memory.

namespace thl::inline THL_ABI_NAMESPACE::midi {

namespace {

constexpr const char* k_log_group = "thl.midi.device_manager";

std::string to_string(CFStringRef string) {
    if (string == nullptr) { return {}; }
    const CFIndex length = CFStringGetLength(string);
    const CFIndex capacity = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
    std::string result(static_cast<std::size_t>(capacity), '\0');
    if (CFStringGetCString(string, result.data(), capacity, kCFStringEncodingUTF8) == 0) {
        return {};
    }
    result.resize(std::char_traits<char>::length(result.c_str()));
    return result;
}

std::string string_property(MIDIObjectRef object, CFStringRef key) {
    CFStringRef value = nullptr;
    if (MIDIObjectGetStringProperty(object, key, &value) != noErr || value == nullptr) {
        return {};
    }
    std::string result = to_string(value);
    CFRelease(value);
    return result;
}

bool integer_property(MIDIObjectRef object, CFStringRef key, SInt32& value) {
    return MIDIObjectGetIntegerProperty(object, key, &value) == noErr;
}

// CoreMIDI stamps packets in mach_absolute_time() ticks; the queue carries nanoseconds in
// the time base of core::uptime_ns(), which is the same clock.
struct TickScale {
    std::uint64_t m_numer = 1;
    std::uint64_t m_denom = 1;

    [[nodiscard]] std::int64_t to_ns(std::uint64_t ticks) const noexcept {
        // Split so that ticks * numer cannot overflow.
        return static_cast<std::int64_t>((ticks / m_denom) * m_numer +
                                         (ticks % m_denom) * m_numer / m_denom);
    }
};

TickScale tick_scale() {
    mach_timebase_info_data_t info{};
    if (mach_timebase_info(&info) != KERN_SUCCESS || info.denom == 0) { return {}; }
    return TickScale{.m_numer = info.numer, .m_denom = info.denom};
}

// The driver that owns the device tells how it is attached. Names as CoreMIDI reports
// them for its own drivers; a vendor's driver stays Unknown.
Transport transport_of(MIDIEndpointRef endpoint) {
    MIDIEntityRef entity = 0;
    if (MIDIEndpointGetEntity(endpoint, &entity) != noErr || entity == 0) {
        return Transport::Virtual;  // an endpoint without an entity is an app's virtual port
    }
    MIDIDeviceRef device = 0;
    if (MIDIEntityGetDevice(entity, &device) != noErr || device == 0) { return Transport::Unknown; }

    const std::string driver = string_property(device, kMIDIPropertyDriverOwner);
    if (driver == "com.apple.AppleMIDIUSBDriver") { return Transport::Usb; }
    if (driver == "com.apple.AppleMIDIBluetoothDriver") { return Transport::Bluetooth; }
    if (driver == "com.apple.AppleMIDINetworkDriver" || driver == "com.apple.AppleMIDIRTPDriver") {
        return Transport::Network;
    }
    if (driver == "com.apple.AppleMIDIIACDriver") { return Transport::Virtual; }
    return Transport::Unknown;
}

DeviceInfo describe(MIDIEndpointRef endpoint) {
    DeviceInfo info;
    info.m_id = endpoint;
    info.m_name = string_property(endpoint, kMIDIPropertyDisplayName);
    if (info.m_name.empty()) { info.m_name = string_property(endpoint, kMIDIPropertyName); }
    info.m_manufacturer = string_property(endpoint, kMIDIPropertyManufacturer);
    info.m_transport = transport_of(endpoint);

    SInt32 protocol = 0;
    if (integer_property(endpoint, kMIDIPropertyProtocolID, protocol) &&
        protocol == kMIDIProtocol_2_0) {
        info.m_protocol = Protocol::Midi2;
    }
    SInt32 unique_id = 0;
    if (integer_property(endpoint, kMIDIPropertyUniqueID, unique_id)) {
        info.m_persistent_key = std::to_string(unique_id);
    }
    return info;
}

// A unique ID derived from the name, so that the virtual input is the same port to other
// apps on every launch (FNV-1a, folded to the positive SInt32 range CoreMIDI uses).
SInt32 stable_unique_id(const std::string& name) {
    std::uint32_t hash = 2166136261U;
    for (const char c : name) {
        hash ^= static_cast<std::uint8_t>(c);
        hash *= 16777619U;
    }
    return static_cast<SInt32>((hash & 0x7FFFFFFFU) | 1U);
}

// CoreMIDI serves a process through the run loop of the thread that created the process's
// first client — for good: device changes reach no client, and its device list does not
// even update, unless that one thread keeps running its run loop, whichever thread the
// later clients are created on (the block API's "arbitrary thread" notwithstanding;
// observed on macOS 26). An app that already has a client created it on a thread with a
// run loop, normally the main thread, and nothing here changes that. Otherwise this
// thread becomes the first: it creates a client it never disposes and runs its run loop
// until the process ends, so a DeviceManager works from any thread, in a command-line
// tool and in a test. A run-loop thread of one's own is what Chromium and JACK2 do; JUCE
// and MIDIKit instead insist on the main thread, RtMidi and libremidi pump the caller's
// run loop on every query (which refreshes the list but delivers no changes).
void ensure_run_loop_thread() {
    static std::once_flag once;
    std::call_once(once, [] {
        std::promise<void> ready;
        std::thread([&ready] {
            pthread_setname_np("thl.midi.runloop");
            MIDIClientRef anchor = 0;
            MIDIClientCreateWithBlock(CFSTR("tanh-midi"), &anchor, nullptr);
            ready.set_value();

            // A run loop without a source returns at once; a timer that never fires in
            // practice keeps it waiting.
            constexpr CFTimeInterval k_far_future = 1.0e10;
            CFRunLoopTimerRef timer =
                CFRunLoopTimerCreateWithHandler(kCFAllocatorDefault,
                                                CFAbsoluteTimeGetCurrent() + k_far_future,
                                                k_far_future,
                                                0,
                                                0,
                                                ^(CFRunLoopTimerRef /*timer*/){});
            CFRunLoopAddTimer(CFRunLoopGetCurrent(), timer, kCFRunLoopCommonModes);
            CFRelease(timer);  // the run loop holds it now
            CFRunLoopRun();
        }).detach();
        ready.get_future().wait();
    });
}

class State : public std::enable_shared_from_this<State> {
public:
    explicit State(BackendContext context)
        : m_context(std::move(context)), m_auto_open(m_context.m_auto_open) {}

    void start() {
        ensure_run_loop_thread();
        m_dispatch_queue = dispatch_queue_create("thl.midi.devices", DISPATCH_QUEUE_SERIAL);

        const CFStringRef name = CFStringCreateWithCString(kCFAllocatorDefault,
                                                           m_context.m_client_name.c_str(),
                                                           kCFStringEncodingUTF8);
        const std::weak_ptr<State> weak = weak_from_this();

        // The notify block runs on the run loop CoreMIDI serves the process through (see
        // ensure_run_loop_thread); the rescan it triggers runs on our serial queue.
        OSStatus status = MIDIClientCreateWithBlock(name, &m_client, ^(const MIDINotification* m) {
            const MIDINotificationMessageID id = m->messageID;
            if (id != kMIDIMsgObjectAdded && id != kMIDIMsgObjectRemoved &&
                id != kMIDIMsgPropertyChanged) {
                return;
            }
            if (const auto self = weak.lock()) { self->schedule_rescan(); }
        });
        if (status != noErr) {
            CFRelease(name);
            m_client = 0;
            Logger::logf(Logger::LogLevel::Warning,
                         k_log_group,
                         "MIDIClientCreateWithBlock failed (%d); no MIDI input",
                         static_cast<int>(status));
            return;
        }

        const std::shared_ptr<InputQueue> queue = m_context.m_queue;
        const bool receive_sysex = m_context.m_receive_sysex;
        const TickScale scale = tick_scale();

        // Runs on CoreMIDI's high-priority receive thread: no locks, no allocation, no
        // logging. The connection's refCon carries the source slot.
        status = MIDIInputPortCreateWithProtocol(
            m_client,
            name,
            protocol_id(),
            &m_port,
            ^(const MIDIEventList* list, void* ref_con) {
                deliver(*queue,
                        list,
                        static_cast<std::uint16_t>(reinterpret_cast<std::uintptr_t>(ref_con)),
                        receive_sysex,
                        scale);
            });
        CFRelease(name);
        if (status != noErr) {
            Logger::logf(Logger::LogLevel::Warning,
                         k_log_group,
                         "MIDIInputPortCreateWithProtocol failed (%d); no MIDI input",
                         static_cast<int>(status));
            MIDIClientDispose(m_client);
            m_client = 0;
            m_port = 0;
            return;
        }

        const std::scoped_lock lock(m_mutex);
        rescan_locked();
    }

    void shut_down() {
        MIDIClientRef client = 0;
        MIDIPortRef port = 0;
        MIDIEndpointRef virtual_endpoint = 0;
        {
            const std::scoped_lock lock(m_mutex);
            m_closed = true;
            client = std::exchange(m_client, 0);
            port = std::exchange(m_port, 0);
            virtual_endpoint = std::exchange(m_virtual_endpoint, 0);
            m_sources.clear();
        }
        if (port != 0) { MIDIPortDispose(port); }
        if (virtual_endpoint != 0) { MIDIEndpointDispose(virtual_endpoint); }
        if (client != 0) { MIDIClientDispose(client); }

        // Wait for a rescan (and the change callback it may be inside) that is under way.
        if (m_dispatch_queue != nullptr) { dispatch_sync(m_dispatch_queue, ^{}); }
    }

    [[nodiscard]] bool running() const {
        const std::scoped_lock lock(m_mutex);
        return m_client != 0;
    }

    [[nodiscard]] std::vector<DeviceInfo> input_devices() const {
        const std::scoped_lock lock(m_mutex);
        std::vector<DeviceInfo> devices;
        devices.reserve(m_sources.size() + 1);
        for (const DeviceInfo& source : m_sources) { devices.push_back(source); }
        if (m_virtual_endpoint != 0) { devices.push_back(m_virtual_info); }
        return devices;
    }

    bool open_input(std::uint32_t id) {
        const std::scoped_lock lock(m_mutex);
        const auto it = find_locked(id);
        if (it == m_sources.end()) { return false; }
        return connect_locked(*it);
    }

    bool close_input(std::uint32_t id) {
        const std::scoped_lock lock(m_mutex);
        const auto it = find_locked(id);
        if (it == m_sources.end()) { return false; }
        if (it->m_open) {
            MIDIPortDisconnectSource(m_port, it->m_id);
            it->m_open = false;
        }
        return true;
    }

    void set_auto_open(bool auto_open) {
        const std::scoped_lock lock(m_mutex);
        m_auto_open = auto_open;
        if (!auto_open) { return; }
        for (DeviceInfo& source : m_sources) { connect_locked(source); }
    }

    bool create_virtual_input(const std::string& name) {
        const std::scoped_lock lock(m_mutex);
        if (m_client == 0 || m_virtual_endpoint != 0) { return false; }

        const std::shared_ptr<InputQueue> queue = m_context.m_queue;
        const bool receive_sysex = m_context.m_receive_sysex;
        const TickScale scale = tick_scale();
        const std::uint16_t source = m_next_source++;

        const CFStringRef cf_name =
            CFStringCreateWithCString(kCFAllocatorDefault, name.c_str(), kCFStringEncodingUTF8);
        MIDIEndpointRef endpoint = 0;
        const OSStatus status = MIDIDestinationCreateWithProtocol(
            m_client,
            cf_name,
            protocol_id(),
            &endpoint,
            ^(const MIDIEventList* list, void* /*ref_con*/) {
                deliver(*queue, list, source, receive_sysex, scale);
            });
        CFRelease(cf_name);
        if (status != noErr) {
            Logger::logf(Logger::LogLevel::Warning,
                         k_log_group,
                         "MIDIDestinationCreateWithProtocol failed (%d); no virtual input",
                         static_cast<int>(status));
            return false;
        }

        // Fails when the ID is taken; the port then keeps the one CoreMIDI gave it.
        MIDIObjectSetIntegerProperty(endpoint, kMIDIPropertyUniqueID, stable_unique_id(name));

        m_virtual_endpoint = endpoint;
        m_virtual_info = describe(endpoint);
        m_virtual_info.m_name = name;
        m_virtual_info.m_transport = Transport::Virtual;
        m_virtual_info.m_protocol = m_context.m_protocol;
        m_virtual_info.m_source = source;
        m_virtual_info.m_open = true;
        return true;
    }

private:
    // The receive path, shared by the input port and the virtual input.
    static void deliver(InputQueue& queue,
                        const MIDIEventList* list,
                        std::uint16_t source,
                        bool receive_sysex,
                        TickScale scale) noexcept {
        const MIDIEventPacket* packet = &list->packet[0];
        for (UInt32 i = 0; i < list->numPackets; ++i) {
            // A timestamp of 0 means "now".
            const std::int64_t time_ns =
                packet->timeStamp == 0 ? core::uptime_ns() : scale.to_ns(packet->timeStamp);

            // The word stream of a packet is whole messages, one after the other.
            const UInt32 word_count = packet->wordCount;
            UInt32 pos = 0;
            while (pos < word_count) {
                const std::uint8_t size = ump_word_count(packet->words[pos]);
                if (word_count - pos < size) { break; }
                Ump ump;
                for (std::uint8_t k = 0; k < size; ++k) { ump.m_words[k] = packet->words[pos + k]; }
                ump.m_size = size;
                if (accepts_packet(ump, receive_sysex)) { queue.push(ump, time_ns, source); }
                pos += size;
            }
            packet = MIDIEventPacketNext(packet);
        }
    }

    [[nodiscard]] MIDIProtocolID protocol_id() const {
        return m_context.m_protocol == Protocol::Midi2 ? kMIDIProtocol_2_0 : kMIDIProtocol_1_0;
    }

    void schedule_rescan() {
        const std::weak_ptr<State> weak = weak_from_this();
        dispatch_async(m_dispatch_queue, ^{
            const auto self = weak.lock();
            if (!self) { return; }
            bool changed = false;
            {
                const std::scoped_lock lock(self->m_mutex);
                if (self->m_closed) { return; }
                changed = self->rescan_locked();
            }
            if (changed && self->m_context.m_on_change) { self->m_context.m_on_change(); }
        });
    }

    [[nodiscard]] std::vector<DeviceInfo>::iterator find_locked(std::uint32_t id) {
        return std::ranges::find_if(m_sources,
                                    [id](const DeviceInfo& source) { return source.m_id == id; });
    }

    bool connect_locked(DeviceInfo& source) {
        if (source.m_open) { return true; }
        if (m_port == 0) { return false; }
        // The refCon is an opaque pointer-sized value CoreMIDI hands back, never a pointer.
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        const auto ref_con = reinterpret_cast<void*>(static_cast<std::uintptr_t>(source.m_source));
        const OSStatus status = MIDIPortConnectSource(m_port, source.m_id, ref_con);
        if (status != noErr) {
            Logger::logf(Logger::LogLevel::Warning,
                         k_log_group,
                         "MIDIPortConnectSource failed (%d) for '%s'",
                         static_cast<int>(status),
                         source.m_name.c_str());
            return false;
        }
        source.m_open = true;
        return true;
    }

    // Brings m_sources in line with the system's sources. A source that is new gets a slot
    // and, with auto-open, a connection; one that stayed keeps its slot and open state;
    // one that went away is dropped (CoreMIDI has already disconnected it). Returns
    // whether the list as input_devices() reports it changed.
    bool rescan_locked() {
        std::vector<DeviceInfo> next;
        const ItemCount count = MIDIGetNumberOfSources();
        next.reserve(count);
        bool changed = false;

        for (ItemCount i = 0; i < count; ++i) {
            const MIDIEndpointRef endpoint = MIDIGetSource(i);
            if (endpoint == 0) { continue; }

            DeviceInfo info = describe(endpoint);
            const auto known = find_locked(endpoint);
            if (known != m_sources.end()) {
                info.m_source = known->m_source;
                info.m_open = known->m_open;
                changed =
                    changed || info.m_name != known->m_name || info.m_protocol != known->m_protocol;
            } else {
                info.m_source = m_next_source++;
                if (m_auto_open) { connect_locked(info); }
                changed = true;
            }
            next.push_back(std::move(info));
        }

        changed = changed || next.size() != m_sources.size();
        m_sources = std::move(next);
        return changed;
    }

    BackendContext m_context;
    mutable std::mutex m_mutex;
    std::vector<DeviceInfo> m_sources;
    DeviceInfo m_virtual_info;
    dispatch_queue_t m_dispatch_queue = nullptr;
    MIDIClientRef m_client = 0;
    MIDIPortRef m_port = 0;
    MIDIEndpointRef m_virtual_endpoint = 0;
    std::uint16_t m_next_source = 0;
    bool m_auto_open = true;
    bool m_closed = false;
};

class AppleBackend final : public DeviceBackend {
public:
    explicit AppleBackend(BackendContext context)
        : m_state(std::make_shared<State>(std::move(context))) {
        m_state->start();
    }
    ~AppleBackend() override { m_state->shut_down(); }

    AppleBackend(const AppleBackend&) = delete;
    AppleBackend& operator=(const AppleBackend&) = delete;
    AppleBackend(AppleBackend&&) = delete;
    AppleBackend& operator=(AppleBackend&&) = delete;

    [[nodiscard]] bool running() const override { return m_state->running(); }
    [[nodiscard]] std::vector<DeviceInfo> input_devices() const override {
        return m_state->input_devices();
    }
    bool open_input(std::uint32_t id) override { return m_state->open_input(id); }
    bool close_input(std::uint32_t id) override { return m_state->close_input(id); }
    void set_auto_open(bool auto_open) override { m_state->set_auto_open(auto_open); }
    bool create_virtual_input(const std::string& name) override {
        return m_state->create_virtual_input(name);
    }

private:
    std::shared_ptr<State> m_state;
};

}  // namespace

bool device_backend_supported() {
    return true;
}

std::unique_ptr<DeviceBackend> make_device_backend(BackendContext context) {
    return std::make_unique<AppleBackend>(std::move(context));
}

}  // namespace thl::inline THL_ABI_NAMESPACE::midi
