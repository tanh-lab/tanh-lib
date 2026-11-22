#include <gtest/gtest.h>
#include <tanh/midi/DeviceManager.h>

#include <vector>

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
