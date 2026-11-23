// The part of the Android backend that needs no Android: bytes of one port in, accepted
// packets in the queue out.
#include <gtest/gtest.h>
#include <tanh/midi/InputQueue.h>

#include <array>
#include <cstdint>
#include <vector>

#include "PortReader.h"

using namespace thl::midi;

namespace {

std::vector<TimedUmp> drain(InputQueue& queue) {
    std::vector<TimedUmp> packets;
    queue.drain([&](const TimedUmp& packet) { packets.push_back(packet); });
    return packets;
}

}  // namespace

// What android.media.midi delivers for an ordinary device: MIDI 1.0 bytes, cut anywhere.
TEST(MidiPortReader, ByteStreamBecomesMidi1Packets) {
    InputQueue queue(16);
    PortReader reader(false);
    const std::array<std::uint8_t, 5> first = {0x90, 60, 100, 0x80, 60};
    const std::array<std::uint8_t, 1> second = {0};

    reader.read(first.data(), first.size(), 1000, 3, false, queue);
    reader.read(second.data(), second.size(), 2000, 3, false, queue);

    const auto packets = drain(queue);
    ASSERT_EQ(packets.size(), 2U);
    EXPECT_EQ(packets[0].m_ump, make_midi1_channel_voice(0, 0x90, 60, 100));
    EXPECT_EQ(packets[0].m_time_ns, 1000);
    EXPECT_EQ(packets[0].m_source, 3);
    // A message completed by a later call carries that call's time.
    EXPECT_EQ(packets[1].m_ump, make_midi1_channel_voice(0, 0x80, 60, 0));
    EXPECT_EQ(packets[1].m_time_ns, 2000);
}

// A device of the Universal MIDI Packet transport: words in network byte order.
TEST(MidiPortReader, UmpBytesBecomeMidi2Packets) {
    InputQueue queue(16);
    PortReader reader(true);
    const std::array<std::uint8_t, 8> bytes = {0x40, 0x90, 0x3C, 0x00, 0xC0, 0x00, 0x00, 0x00};

    reader.read(bytes.data(), bytes.size(), 5, 1, false, queue);

    const auto packets = drain(queue);
    ASSERT_EQ(packets.size(), 1U);
    EXPECT_EQ(packets[0].m_ump,
              make_midi2_channel_voice(0, ChannelVoiceOpcode::NoteOn, 0, 60, 0, 0xC0000000U));
}

TEST(MidiPortReader, SysExOnlyOnRequest) {
    const std::array<std::uint8_t, 8> bytes = {0xF0, 0x7E, 0x7F, 0xF7, 0x90, 60, 100, 0xF8};

    InputQueue without(16);
    PortReader(false).read(bytes.data(), bytes.size(), 0, 0, false, without);
    const auto filtered = drain(without);
    ASSERT_EQ(filtered.size(), 2U);
    EXPECT_EQ(filtered[0].m_ump, make_midi1_channel_voice(0, 0x90, 60, 100));
    EXPECT_EQ(filtered[1].m_ump, make_system(0, 0xF8));

    InputQueue with(16);
    PortReader(false).read(bytes.data(), bytes.size(), 0, 0, true, with);
    const auto all = drain(with);
    ASSERT_EQ(all.size(), 3U);
    EXPECT_EQ(all[0].m_ump.message_type(), MessageType::SysEx7);
}

// Transport housekeeping of a UMP device never reaches the queue.
TEST(MidiPortReader, DropsUtilityAndStreamMessages) {
    InputQueue queue(16);
    PortReader reader(true);
    const std::array<std::uint8_t, 24> bytes = {
        0x00, 0x20, 0x12, 0x34,                                      // jitter-reduction timestamp
        0xF0, 0x00, 0x01, 0x01, 0,    0,    0,    0,    0, 0, 0, 0,  // UMP stream message, 4 words
        0,    0,    0,    0,    0x20, 0x90, 0x3C, 0x64,              // MIDI 1.0 note on
    };

    reader.read(bytes.data(), bytes.size(), 0, 0, true, queue);

    const auto packets = drain(queue);
    ASSERT_EQ(packets.size(), 1U);
    EXPECT_EQ(packets[0].m_ump, make_midi1_channel_voice(0, 0x90, 60, 100));
}

// Reopening a port must not finish a message the previous session left half-read.
TEST(MidiPortReader, ResetForgetsAPartialMessage) {
    InputQueue queue(16);
    PortReader reader(false);
    const std::array<std::uint8_t, 2> partial = {0x90, 60};
    const std::array<std::uint8_t, 3> next = {100, 0xB0, 7};
    const std::array<std::uint8_t, 1> value = {64};

    reader.read(partial.data(), partial.size(), 0, 0, false, queue);
    reader.reset();
    reader.read(next.data(), next.size(), 0, 0, false, queue);
    EXPECT_TRUE(drain(queue).empty());
    reader.read(value.data(), value.size(), 0, 0, false, queue);

    const auto packets = drain(queue);
    ASSERT_EQ(packets.size(), 1U);
    EXPECT_EQ(packets[0].m_ump, make_midi1_channel_voice(0, 0xB0, 7, 64));
}
