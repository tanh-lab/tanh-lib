#include <gtest/gtest.h>
#include <tanh/midi/AndroidMidi.h>
#include <tanh/midi/DeviceManager.h>

#include <cstdint>
#include <vector>

#include "DeviceBackend.h"

using namespace thl::midi;

// The manager's queue is usable on every platform, with or without a device backend: it
// is also the way in for MIDI the application gets from elsewhere (a plugin host).
TEST(MidiDeviceManager, QueueCarriesPushedPackets) {
    DeviceManager manager;
    manager.input_queue().push(make_midi1_channel_voice(0, 0x90, 60, 100), 42, 7);

    std::vector<TimedUmp> packets;
    manager.input_queue().drain([&](const TimedUmp& packet) { packets.push_back(packet); });
    ASSERT_EQ(packets.size(), 1U);
    EXPECT_EQ(packets[0].m_ump, make_midi1_channel_voice(0, 0x90, 60, 100));
    EXPECT_EQ(packets[0].m_time_ns, 42);
    EXPECT_EQ(packets[0].m_source, 7);
}

TEST(MidiDeviceManager, QueueCapacityComesFromTheConfig) {
    DeviceManagerConfig config;
    config.m_queue_capacity = 64;
    DeviceManager manager(config);
    EXPECT_EQ(manager.input_queue().capacity(), 64U);
}

// Without a backend nothing is listed and nothing opens, and every call still returns.
TEST(MidiDeviceManager, UnsupportedPlatformReportsNothing) {
    if (DeviceManager::supported()) { GTEST_SKIP() << "this platform has a device backend"; }

    DeviceManager manager;
    EXPECT_FALSE(manager.running());
    EXPECT_TRUE(manager.input_devices().empty());
    EXPECT_FALSE(manager.open_input(1));
    EXPECT_FALSE(manager.close_input(1));
    EXPECT_FALSE(manager.create_virtual_input("tanh"));
    manager.set_auto_open(true);
    manager.set_device_change_callback([] {});
}

// The change callback can be replaced and taken away again at any time.
TEST(MidiDeviceManager, ChangeCallbackCanBeSetAndCleared) {
    DeviceManager manager;
    manager.set_device_change_callback([] {});
    manager.set_device_change_callback([] {});
    manager.set_device_change_callback({});
}

#if !defined(THL_PLATFORM_ANDROID)
// Callers need no #ifdef: off Android the call does nothing.
TEST(MidiDeviceManager, SetAndroidJavaVmIsANoOpElsewhere) {
    set_android_java_vm(nullptr);
    DeviceManager manager;
    EXPECT_EQ(manager.running(), DeviceManager::supported());
}
#endif

// What a backend lets into the queue: performance data always, System Exclusive on
// request, transport housekeeping and reserved packets never.
TEST(MidiDeviceBackend, AcceptsPerformanceDataOnly) {
    const auto of_type = [](std::uint32_t type) {
        Ump ump;
        ump.m_words[0] = type << 28U;
        ump.m_size = ump_word_count(ump.m_words[0]);
        return ump;
    };

    for (const bool sysex : {false, true}) {
        for (const std::uint32_t type : {0x1U, 0x2U, 0x4U}) {  // system, MIDI 1.0, MIDI 2.0
            EXPECT_TRUE(accepts_packet(of_type(type), sysex)) << "message type " << type;
        }
        for (const std::uint32_t type : {0x3U, 0x5U}) {  // System Exclusive, 7 and 8 bit
            EXPECT_EQ(accepts_packet(of_type(type), sysex), sysex) << "message type " << type;
        }
        // Utility, flex data, UMP stream, and the reserved types.
        for (const std::uint32_t type :
             {0x0U, 0xDU, 0xFU, 0x6U, 0x7U, 0x8U, 0x9U, 0xAU, 0xBU, 0xCU, 0xEU}) {
            EXPECT_FALSE(accepts_packet(of_type(type), sysex)) << "message type " << type;
        }
    }
}
