// DeviceManager against the real CoreMIDI: the tests create virtual ports in this process
// and send through them, so no hardware is needed. They skip where the MIDI service is not
// available (a headless machine).
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#include <gtest/gtest.h>
#include <mach/mach_time.h>
#include <tanh/core/BufferView.h>
#include <tanh/core/Clock.h>
#include <tanh/midi/AudioMidiProcessor.h>
#include <tanh/midi/DeviceManager.h>
#include <tanh/midi/Midi1Converter.h>
#include <unistd.h>

#if defined(THL_TEST_WITH_AUDIO_IO)
#include <tanh/midi/AudioMidiCallback.h>
#endif

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

using namespace thl::midi;
using namespace std::chrono_literals;

namespace {

constexpr auto k_timeout = 3s;
constexpr auto k_quiet = 200ms;  // how long to wait for something that must not arrive

bool wait_for(const std::function<bool()>& condition,
              std::chrono::milliseconds timeout = k_timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) { return true; }
        std::this_thread::sleep_for(5ms);
    }
    return condition();
}

std::string unique_name(const char* what) {
    static std::atomic<int> counter{0};
    return std::string("tanh-test-") + what + "-" + std::to_string(getpid()) + "-" +
           std::to_string(counter.fetch_add(1));
}

CFStringRef make_cf_string(const std::string& string) {
    return CFStringCreateWithCString(kCFAllocatorDefault, string.c_str(), kCFStringEncodingUTF8);
}

// The test's own CoreMIDI client: the "other app" the manager receives from.
class TestClient {
public:
    TestClient() {
        const CFStringRef name = make_cf_string(unique_name("client"));
        if (MIDIClientCreate(name, nullptr, nullptr, &m_client) != noErr) { m_client = 0; }
        if (m_client != 0 && MIDIOutputPortCreate(m_client, name, &m_output_port) != noErr) {
            m_output_port = 0;
        }
        CFRelease(name);
    }
    ~TestClient() {
        if (m_client != 0) { MIDIClientDispose(m_client); }
    }
    TestClient(const TestClient&) = delete;
    TestClient& operator=(const TestClient&) = delete;

    [[nodiscard]] bool valid() const { return m_client != 0 && m_output_port != 0; }
    [[nodiscard]] MIDIClientRef client() const { return m_client; }
    [[nodiscard]] MIDIPortRef output_port() const { return m_output_port; }

private:
    MIDIClientRef m_client = 0;
    MIDIPortRef m_output_port = 0;
};

void fill_event_list(MIDIEventList& list,
                     MIDIProtocolID protocol,
                     std::initializer_list<std::uint32_t> words) {
    const std::vector<std::uint32_t> data(words);
    MIDIEventPacket* packet = MIDIEventListInit(&list, protocol);
    packet = MIDIEventListAdd(&list, sizeof(MIDIEventList), packet, 0, data.size(), data.data());
    ASSERT_NE(packet, nullptr);
}

// A virtual source: appears to the manager as an input another app provides.
class VirtualSource {
public:
    VirtualSource(const TestClient& client, MIDIProtocolID protocol)
        : m_name(unique_name("source")), m_protocol(protocol) {
        const CFStringRef name = make_cf_string(m_name);
        if (MIDISourceCreateWithProtocol(client.client(), name, protocol, &m_endpoint) != noErr) {
            m_endpoint = 0;
        }
        CFRelease(name);
    }
    ~VirtualSource() { dispose(); }
    VirtualSource(const VirtualSource&) = delete;
    VirtualSource& operator=(const VirtualSource&) = delete;

    void dispose() {
        if (m_endpoint != 0) { MIDIEndpointDispose(m_endpoint); }
        m_endpoint = 0;
    }

    void send(std::initializer_list<std::uint32_t> words) const {
        MIDIEventList list{};
        fill_event_list(list, m_protocol, words);
        ASSERT_EQ(MIDIReceivedEventList(m_endpoint, &list), noErr);
    }

    // One list, one two-word packet per entry, each with its own timestamp (host ticks).
    void send_stamped(const std::vector<std::array<std::uint32_t, 2>>& messages,
                      const std::vector<MIDITimeStamp>& timestamps) const {
        MIDIEventList list{};
        MIDIEventPacket* packet = MIDIEventListInit(&list, m_protocol);
        for (std::size_t i = 0; i < messages.size(); ++i) {
            packet = MIDIEventListAdd(&list,
                                      sizeof(MIDIEventList),
                                      packet,
                                      timestamps[i],
                                      messages[i].size(),
                                      messages[i].data());
            ASSERT_NE(packet, nullptr);
        }
        ASSERT_EQ(MIDIReceivedEventList(m_endpoint, &list), noErr);
    }

    [[nodiscard]] bool valid() const { return m_endpoint != 0; }
    [[nodiscard]] const std::string& name() const { return m_name; }

private:
    std::string m_name;
    MIDIProtocolID m_protocol;
    MIDIEndpointRef m_endpoint = 0;
};

std::optional<DeviceInfo> find_device(const DeviceManager& manager, const std::string& name) {
    for (const DeviceInfo& device : manager.input_devices()) {
        if (device.m_name == name) { return device; }
    }
    return std::nullopt;
}

bool listed(const DeviceManager& manager, const std::string& name) {
    return wait_for([&] { return find_device(manager, name).has_value(); });
}

// Packets that have arrived from one source slot; other inputs of the machine (a
// keyboard that happens to be plugged in) are ignored.
std::vector<TimedUmp> receive(DeviceManager& manager,
                              std::uint16_t source,
                              std::size_t count,
                              std::chrono::milliseconds timeout = k_timeout) {
    std::vector<TimedUmp> packets;
    wait_for(
        [&] {
            manager.input_queue().drain([&](const TimedUmp& packet) {
                if (packet.m_source == source) { packets.push_back(packet); }
            });
            return packets.size() >= count;
        },
        timeout);
    return packets;
}

std::vector<std::uint8_t> to_midi1_bytes(const std::vector<TimedUmp>& packets) {
    std::vector<std::uint8_t> bytes;
    for (const TimedUmp& packet : packets) {
        Midi1Messages messages;
        const std::size_t count = convert_to_midi1(packet.m_ump, messages);
        for (std::size_t i = 0; i < count; ++i) {
            bytes.insert(bytes.end(),
                         messages[i].m_bytes.begin(),
                         messages[i].m_bytes.begin() + messages[i].m_size);
        }
    }
    return bytes;
}

class CoreMidi : public ::testing::Test {
protected:
    // CoreMIDI serves a process through the run loop of the thread that makes its first
    // CoreMIDI call. The test thread runs none, so a manager goes first: its backend
    // brings a run-loop thread of its own, and the test clients created afterwards ride
    // on it.
    static void SetUpTestSuite() { const DeviceManager first; }

    void SetUp() override {
        if (!m_client.valid()) { GTEST_SKIP() << "CoreMIDI is not available on this machine"; }
    }

    TestClient m_client;
};

}  // namespace

TEST_F(CoreMidi, ReceivesFromAnInputThatIsPresent) {
    const VirtualSource source(m_client, kMIDIProtocol_2_0);
    ASSERT_TRUE(source.valid());

    DeviceManager manager;
    ASSERT_TRUE(DeviceManager::supported());
    ASSERT_TRUE(manager.running());
    ASSERT_TRUE(listed(manager, source.name()));

    const DeviceInfo device = *find_device(manager, source.name());
    EXPECT_TRUE(device.m_open);  // auto-open is the default
    EXPECT_EQ(device.m_transport, Transport::Virtual);
    EXPECT_EQ(device.m_protocol, Protocol::Midi2);
    EXPECT_FALSE(device.m_persistent_key.empty());

    const std::int64_t before = thl::core::uptime_ns();
    source.send({0x40903C00U, 0xC0000000U});  // MIDI 2.0 note on, 16-bit velocity 0xC000
    const auto packets = receive(manager, device.m_source, 1);
    const std::int64_t after = thl::core::uptime_ns();

    ASSERT_EQ(packets.size(), 1U);
    EXPECT_EQ(packets[0].m_ump,
              make_midi2_channel_voice(0, ChannelVoiceOpcode::NoteOn, 0, 60, 0, 0xC0000000U));
    // The arrival time is in the time base of uptime_ns().
    EXPECT_GE(packets[0].m_time_ns, before);
    EXPECT_LE(packets[0].m_time_ns, after);
    EXPECT_EQ(manager.input_queue().dropped(), 0U);
}

// Several messages in one event list arrive as separate packets, in order.
TEST_F(CoreMidi, SplitsAnEventListIntoPackets) {
    const VirtualSource source(m_client, kMIDIProtocol_2_0);
    DeviceManager manager;
    ASSERT_TRUE(listed(manager, source.name()));
    const std::uint16_t slot = find_device(manager, source.name())->m_source;

    source.send({0x40903C00U, 0x80000000U, 0x10F80000U, 0x40803C00U, 0x00000000U});
    const auto packets = receive(manager, slot, 3);

    ASSERT_EQ(packets.size(), 3U);
    EXPECT_EQ(packets[0].m_ump.opcode(), ChannelVoiceOpcode::NoteOn);
    EXPECT_EQ(packets[1].m_ump, make_system(0, 0xF8));
    EXPECT_EQ(packets[2].m_ump.opcode(), ChannelVoiceOpcode::NoteOff);
}

// What CoreMIDI makes of a MIDI 1.0 source on a MIDI 2.0 port: MIDI 2.0 channel voice
// messages that scale back to exactly the bytes that were sent.
TEST_F(CoreMidi, Midi1SourceArrivesAsMidi2OnAMidi2Port) {
    const VirtualSource source(m_client, kMIDIProtocol_1_0);
    DeviceManager manager;  // Protocol::Midi2
    ASSERT_TRUE(listed(manager, source.name()));
    const DeviceInfo device = *find_device(manager, source.name());
    EXPECT_EQ(device.m_protocol, Protocol::Midi1);

    source.send({
        0x20903C64U,  // note on 60, velocity 100
        0x20B14A7FU,  // control change 74 = 127 on channel 1
        0x20E20040U,  // pitch bend centre on channel 2
        0x20803C00U,  // note off 60
    });
    const auto packets = receive(manager, device.m_source, 4);

    ASSERT_EQ(packets.size(), 4U);
    for (const TimedUmp& packet : packets) {
        EXPECT_EQ(packet.m_ump.message_type(), MessageType::Midi2ChannelVoice);
    }
    EXPECT_EQ(
        to_midi1_bytes(packets),
        (std::vector<std::uint8_t>{0x90, 60, 100, 0xB1, 74, 127, 0xE2, 0x00, 0x40, 0x80, 60, 0}));
}

// A consumer that asks for MIDI 1.0 gets MIDI 1.0 channel voice messages even from a
// MIDI 2.0 source.
TEST_F(CoreMidi, Midi1ProtocolDeliversMidi1) {
    const VirtualSource source(m_client, kMIDIProtocol_2_0);
    DeviceManagerConfig config;
    config.m_protocol = Protocol::Midi1;
    DeviceManager manager(config);
    ASSERT_TRUE(listed(manager, source.name()));
    const std::uint16_t slot = find_device(manager, source.name())->m_source;

    source.send({0x40913C00U, 0xC0000000U});  // MIDI 2.0 note on, channel 1
    const auto packets = receive(manager, slot, 1);

    ASSERT_EQ(packets.size(), 1U);
    EXPECT_EQ(packets[0].m_ump, make_midi1_channel_voice(0, 0x91, 60, 0x60));
}

TEST_F(CoreMidi, FollowsHotPlug) {
    DeviceManager manager;
    std::atomic<int> changes{0};
    manager.set_device_change_callback([&] { changes.fetch_add(1); });

    VirtualSource source(m_client, kMIDIProtocol_2_0);
    ASSERT_TRUE(listed(manager, source.name()));
    EXPECT_TRUE(wait_for([&] { return changes.load() >= 1; }));

    // An input that appears later is opened and delivers.
    const DeviceInfo device = *find_device(manager, source.name());
    EXPECT_TRUE(device.m_open);
    source.send({0x40903C00U, 0x80000000U});
    EXPECT_EQ(receive(manager, device.m_source, 1).size(), 1U);

    const int before = changes.load();
    source.dispose();
    EXPECT_TRUE(wait_for([&] { return !find_device(manager, source.name()).has_value(); }));
    EXPECT_TRUE(wait_for([&] { return changes.load() > before; }));

    // A cleared callback is not called again.
    manager.set_device_change_callback({});
    const int cleared = changes.load();
    const VirtualSource another(m_client, kMIDIProtocol_2_0);
    ASSERT_TRUE(listed(manager, another.name()));
    EXPECT_EQ(changes.load(), cleared);
}

// The managers of a process share one MIDI client, which stays when they go: each one
// follows hot-plug and receives on its own, and the ones that are left go on working.
TEST_F(CoreMidi, ManagersShareTheClientAndWorkOnTheirOwn) {
    DeviceManager first;
    auto second = std::make_unique<DeviceManager>();
    std::atomic<int> first_changes{0};
    std::atomic<int> second_changes{0};
    first.set_device_change_callback([&] { first_changes.fetch_add(1); });
    second->set_device_change_callback([&] { second_changes.fetch_add(1); });

    const VirtualSource source(m_client, kMIDIProtocol_2_0);
    ASSERT_TRUE(listed(first, source.name()));
    ASSERT_TRUE(listed(*second, source.name()));
    EXPECT_TRUE(wait_for([&] { return first_changes.load() >= 1 && second_changes.load() >= 1; }));

    // Both are connected to the input, each with its own queue.
    source.send({0x40903C00U, 0x80000000U});
    EXPECT_EQ(receive(first, find_device(first, source.name())->m_source, 1).size(), 1U);
    EXPECT_EQ(receive(*second, find_device(*second, source.name())->m_source, 1).size(), 1U);

    // One goes; the other still follows hot-plug and receives.
    second.reset();
    const VirtualSource later(m_client, kMIDIProtocol_2_0);
    ASSERT_TRUE(listed(first, later.name()));
    later.send({0x40903E00U, 0x80000000U});
    EXPECT_EQ(receive(first, find_device(first, later.name())->m_source, 1).size(), 1U);
}

// A manager can be created and destroyed any number of times (the client is not disposed
// with it, which CoreMIDI does not take well when repeated).
TEST_F(CoreMidi, ManagersCanComeAndGoManyTimes) {
    const VirtualSource source(m_client, kMIDIProtocol_2_0);
    for (int i = 0; i < 64; ++i) {
        DeviceManager manager;
        ASSERT_TRUE(manager.running()) << "manager " << i;
        if (i % 16 == 0) { manager.create_virtual_input(unique_name("input")); }
    }

    DeviceManager manager;
    ASSERT_TRUE(listed(manager, source.name()));
    source.send({0x40903C00U, 0x80000000U});
    EXPECT_EQ(receive(manager, find_device(manager, source.name())->m_source, 1).size(), 1U);

    const VirtualSource later(m_client, kMIDIProtocol_2_0);
    EXPECT_TRUE(listed(manager, later.name()));
}

TEST_F(CoreMidi, WithoutAutoOpenAnInputDeliversOnlyWhileOpen) {
    const VirtualSource source(m_client, kMIDIProtocol_2_0);
    DeviceManagerConfig config;
    config.m_auto_open = false;
    DeviceManager manager(config);
    ASSERT_TRUE(listed(manager, source.name()));
    const DeviceInfo device = *find_device(manager, source.name());
    EXPECT_FALSE(device.m_open);

    source.send({0x40903C00U, 0x80000000U});
    EXPECT_TRUE(receive(manager, device.m_source, 1, k_quiet).empty());

    EXPECT_TRUE(manager.open_input(device.m_id));
    EXPECT_TRUE(find_device(manager, source.name())->m_open);
    source.send({0x40903D00U, 0x80000000U});
    const auto packets = receive(manager, device.m_source, 1);
    ASSERT_EQ(packets.size(), 1U);
    EXPECT_EQ(packets[0].m_ump.data1(), 61);

    EXPECT_TRUE(manager.close_input(device.m_id));
    EXPECT_FALSE(find_device(manager, source.name())->m_open);
    source.send({0x40903E00U, 0x80000000U});
    EXPECT_TRUE(receive(manager, device.m_source, 1, k_quiet).empty());

    EXPECT_FALSE(manager.open_input(0xFFFFFFF0U));  // no such input
    EXPECT_FALSE(manager.close_input(0xFFFFFFF0U));

    // Turning auto-open on opens what is already there.
    manager.set_auto_open(true);
    EXPECT_TRUE(find_device(manager, source.name())->m_open);
}

// System Exclusive stays out of the queue unless asked for; the notes around it arrive.
TEST_F(CoreMidi, SysExOnlyOnRequest) {
    const VirtualSource source(m_client, kMIDIProtocol_2_0);
    const std::initializer_list<std::uint32_t> words = {
        0x30047E7FU,
        0x06010000U,  // complete SysEx7 with four bytes
        0x40903C00U,
        0x80000000U,  // note on
    };

    {
        DeviceManager manager;
        ASSERT_TRUE(listed(manager, source.name()));
        const std::uint16_t slot = find_device(manager, source.name())->m_source;
        source.send(words);
        const auto packets = receive(manager, slot, 1);
        ASSERT_EQ(packets.size(), 1U);
        EXPECT_EQ(packets[0].m_ump.message_type(), MessageType::Midi2ChannelVoice);
        EXPECT_TRUE(receive(manager, slot, 1, k_quiet).empty());
    }
    {
        DeviceManagerConfig config;
        config.m_receive_sysex = true;
        DeviceManager manager(config);
        ASSERT_TRUE(listed(manager, source.name()));
        const std::uint16_t slot = find_device(manager, source.name())->m_source;
        source.send(words);
        const auto packets = receive(manager, slot, 2);
        ASSERT_EQ(packets.size(), 2U);
        EXPECT_EQ(packets[0].m_ump.message_type(), MessageType::SysEx7);
        EXPECT_EQ(sysex7_byte_count(packets[0].m_ump), 4);
        EXPECT_EQ(sysex7_byte(packets[0].m_ump, 0), 0x7E);
    }
}

// The manager's own virtual input: a destination other apps can send to.
TEST_F(CoreMidi, VirtualInputReceivesFromOtherClients) {
    DeviceManager manager;
    const std::string name = unique_name("input");
    if (!manager.create_virtual_input(name)) {
        GTEST_SKIP() << "the system does not allow a virtual MIDI destination here";
    }
    EXPECT_FALSE(manager.create_virtual_input(unique_name("second")));  // one per manager

    const std::optional<DeviceInfo> device = find_device(manager, name);
    ASSERT_TRUE(device.has_value());
    EXPECT_TRUE(device->m_open);
    EXPECT_EQ(device->m_transport, Transport::Virtual);

    // Find the destination the way another app would, by its name.
    MIDIEndpointRef destination = 0;
    ASSERT_TRUE(wait_for([&] {
        const ItemCount count = MIDIGetNumberOfDestinations();
        for (ItemCount i = 0; i < count; ++i) {
            const MIDIEndpointRef endpoint = MIDIGetDestination(i);
            CFStringRef cf_name = nullptr;
            if (MIDIObjectGetStringProperty(endpoint, kMIDIPropertyName, &cf_name) != noErr) {
                continue;
            }
            std::array<char, 256> buffer{};
            CFStringGetCString(cf_name, buffer.data(), buffer.size(), kCFStringEncodingUTF8);
            CFRelease(cf_name);
            if (name == buffer.data()) {
                destination = endpoint;
                return true;
            }
        }
        return false;
    }));

    MIDIEventList list{};
    fill_event_list(list, kMIDIProtocol_2_0, {0x40903C00U, 0xFFFF0000U});
    ASSERT_EQ(MIDISendEventList(m_client.output_port(), destination, &list), noErr);

    const auto packets = receive(manager, device->m_source, 1);
    ASSERT_EQ(packets.size(), 1U);
    EXPECT_EQ(packets[0].m_ump,
              make_midi2_channel_voice(0, ChannelVoiceOpcode::NoteOn, 0, 60, 0, 0xFFFF0000U));

    // It closes and opens like any other input; the port stays where other apps see it.
    EXPECT_TRUE(manager.close_input(device->m_id));
    EXPECT_FALSE(find_device(manager, name)->m_open);
    ASSERT_EQ(MIDISendEventList(m_client.output_port(), destination, &list), noErr);
    EXPECT_TRUE(receive(manager, device->m_source, 1, k_quiet).empty());

    EXPECT_TRUE(manager.open_input(device->m_id));
    EXPECT_TRUE(find_device(manager, name)->m_open);
    ASSERT_EQ(MIDISendEventList(m_client.output_port(), destination, &list), noErr);
    EXPECT_EQ(receive(manager, device->m_source, 1).size(), 1U);
}

// Destroying the manager while inputs are open and data is flowing must be clean.
TEST_F(CoreMidi, CanBeDestroyedWhileReceiving) {
    const VirtualSource source(m_client, kMIDIProtocol_2_0);
    for (int round = 0; round < 5; ++round) {
        DeviceManager manager;
        manager.set_device_change_callback([] {});
        ASSERT_TRUE(listed(manager, source.name()));
        for (int i = 0; i < 50; ++i) { source.send({0x40903C00U, 0x80000000U}); }
    }
}

// AudioMidiCallback joins Midi and AudioIO; a build without AudioIO has no such test.
#if defined(THL_TEST_WITH_AUDIO_IO)

namespace {

// Notes where it finds them in the stream: (note number, absolute frame).
class NoteLogger final : public thl::AudioMidiProcessor {
public:
    void prepare(double /*sample_rate*/,
                 std::size_t /*max_block_size*/,
                 std::size_t /*num_channels*/) override {}

    void process(thl::core::BufferView audio, std::span<const thl::BlockEvent> events) override {
        for (const thl::BlockEvent& event : events) {
            if (event.m_source == m_source) {
                m_notes.emplace_back(event.m_midi.data1(), m_position + event.m_sample_offset);
            }
        }
        m_position += audio.get_num_samples();
    }

    std::uint16_t m_source = 0;
    std::uint64_t m_position = 0;
    std::vector<std::pair<int, std::uint64_t>> m_notes;
};

}  // namespace

// The whole chain on real clocks: CoreMIDI's own timestamps, the queue, and an audio
// stream paced by the system clock (this thread plays the audio device). Notes stamped
// 7.3 ms apart must come out of the engine 7.3 ms of samples apart.
TEST_F(CoreMidi, TimestampsLineUpWithTheAudioStream) {
    constexpr std::uint32_t k_block = 480;
    constexpr double k_sample_rate = 48000.0;
    constexpr double k_spacing_ms = 7.3;
    constexpr int k_notes = 5;

    const VirtualSource source(m_client, kMIDIProtocol_2_0);
    DeviceManager manager;
    ASSERT_TRUE(listed(manager, source.name()));

    NoteLogger engine;
    engine.m_source = find_device(manager, source.name())->m_source;
    thl::AudioMidiCallback callback(engine, manager.input_queue(), 2);
    callback.prepare_to_play(static_cast<std::uint32_t>(k_sample_rate), k_block);

    mach_timebase_info_data_t timebase{};
    ASSERT_EQ(mach_timebase_info(&timebase), KERN_SUCCESS);
    const auto ticks_of_ms = [&](double ms) {
        return static_cast<MIDITimeStamp>(ms * 1.0e6 * timebase.denom / timebase.numer);
    };

    std::vector<float> output(static_cast<std::size_t>(k_block) * 2);
    const auto start = std::chrono::steady_clock::now();
    for (int block = 0; block < 60; ++block) {
        std::this_thread::sleep_until(start + block * 10ms);
        if (block == 10) {
            // Five notes, the first 30 ms from now: stamped ahead, as a sequencer would.
            std::vector<std::array<std::uint32_t, 2>> messages;
            std::vector<MIDITimeStamp> timestamps;
            const MIDITimeStamp base = mach_absolute_time() + ticks_of_ms(30.0);
            for (int i = 0; i < k_notes; ++i) {
                messages.push_back(
                    {0x40900000U | (static_cast<std::uint32_t>(60 + i) << 8U), 0x80000000U});
                timestamps.push_back(base + ticks_of_ms(i * k_spacing_ms));
            }
            source.send_stamped(messages, timestamps);
        }
        callback.process(output.data(), nullptr, k_block, 0, 2);
    }

    ASSERT_EQ(engine.m_notes.size(), static_cast<std::size_t>(k_notes));
    const double spacing_frames = k_spacing_ms * k_sample_rate / 1000.0;  // 350.4
    for (int i = 0; i < k_notes; ++i) {
        EXPECT_EQ(engine.m_notes[static_cast<std::size_t>(i)].first, 60 + i);
        if (i == 0) { continue; }
        const double distance =
            static_cast<double>(engine.m_notes[static_cast<std::size_t>(i)].second) -
            static_cast<double>(engine.m_notes[static_cast<std::size_t>(i - 1)].second);
        // To the sample on a steady stream (MidiStreamClock.*); here the "device" is a
        // sleeping thread, so allow for its wake-up jitter.
        EXPECT_NEAR(distance, spacing_frames, 96.0) << "between notes " << i - 1 << " and " << i;
    }
}

#endif  // THL_TEST_WITH_AUDIO_IO
