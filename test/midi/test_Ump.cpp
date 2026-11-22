#include <gtest/gtest.h>
#include <tanh/midi/Ump.h>
#include <tanh/midi/UmpByteReader.h>

#include <array>
#include <cstdint>
#include <vector>

using namespace thl::midi;

// The sizes the UMP specification assigns to every message type, reserved ones included.
TEST(MidiUmp, WordCountFollowsMessageType) {
    const std::array<std::uint8_t, 16> expected = {1, 1, 1, 2, 2, 4, 1, 1, 2, 2, 2, 3, 3, 4, 4, 4};
    for (std::uint32_t type = 0; type < 16; ++type) {
        EXPECT_EQ(ump_word_count(type << 28U), expected[type]) << "message type " << type;
        // Only the top nibble decides.
        EXPECT_EQ(ump_word_count((type << 28U) | 0x0FFFFFFFU), expected[type]);
    }
}

TEST(MidiUmp, Midi1ChannelVoiceLayout) {
    const Ump ump = make_midi1_channel_voice(3, 0x95, 60, 100);
    EXPECT_EQ(ump.m_size, 1);
    EXPECT_EQ(ump.m_words[0], 0x23953C64U);
    EXPECT_EQ(ump.message_type(), MessageType::Midi1ChannelVoice);
    EXPECT_EQ(ump.group(), 3);
    EXPECT_EQ(ump.status(), 0x95);
    EXPECT_EQ(ump.opcode(), ChannelVoiceOpcode::NoteOn);
    EXPECT_EQ(ump.channel(), 5);
    EXPECT_EQ(ump.data1(), 60);
    EXPECT_EQ(ump.data2(), 100);
}

TEST(MidiUmp, Midi2ChannelVoiceLayout) {
    const Ump ump =
        make_midi2_channel_voice(1, ChannelVoiceOpcode::NoteOn, 2, 60, 0x03, 0xC0001234U);
    EXPECT_EQ(ump.m_size, 2);
    EXPECT_EQ(ump.m_words[0], 0x41923C03U);
    EXPECT_EQ(ump.m_words[1], 0xC0001234U);
    EXPECT_EQ(ump.message_type(), MessageType::Midi2ChannelVoice);
    EXPECT_EQ(ump.opcode(), ChannelVoiceOpcode::NoteOn);
    EXPECT_EQ(ump.channel(), 2);
    EXPECT_EQ(ump.data1(), 60);
    EXPECT_EQ(ump.data2(), 0x03);
}

TEST(MidiUmp, SystemLayout) {
    const Ump clock = make_system(0, 0xF8);
    EXPECT_EQ(clock.m_words[0], 0x10F80000U);
    EXPECT_EQ(clock.message_type(), MessageType::System);
    EXPECT_EQ(clock.status(), 0xF8);

    const Ump position = make_system(0, 0xF2, 0x12, 0x34);
    EXPECT_EQ(position.m_words[0], 0x10F21234U);
}

TEST(MidiUmp, SysEx7Payload) {
    const std::array<std::uint8_t, 6> bytes = {0x7E, 0x7F, 0x06, 0x01, 0x00, 0x00};
    const Ump ump = make_sysex7(0, SysEx7Status::Complete, bytes, 4);
    EXPECT_EQ(ump.m_size, 2);
    EXPECT_EQ(ump.m_words[0], 0x30047E7FU);
    EXPECT_EQ(ump.m_words[1], 0x06010000U);
    EXPECT_EQ(sysex7_byte_count(ump), 4);
    for (std::uint8_t i = 0; i < 6; ++i) { EXPECT_EQ(sysex7_byte(ump, i), bytes[i]) << int(i); }
}

// Unused words are zero, so equality is equality of the message.
TEST(MidiUmp, EqualityComparesTheMessage) {
    EXPECT_EQ(make_midi1_channel_voice(0, 0x90, 60, 100),
              make_midi1_channel_voice(0, 0x90, 60, 100));
    EXPECT_NE(make_midi1_channel_voice(0, 0x90, 60, 100),
              make_midi1_channel_voice(0, 0x90, 60, 101));
    EXPECT_NE(make_midi1_channel_voice(0, 0x90, 60, 100),
              make_midi1_channel_voice(1, 0x90, 60, 100));
}

TEST(MidiUmp, ForEachUmpSplitsAWordStream) {
    const std::array<std::uint32_t, 8> words = {
        0x20903C64U,  // MIDI 1.0 note on
        0x40903C00U,
        0x80000000U,  // MIDI 2.0 note on
        0x10F80000U,  // clock
        0x50000000U,
        1U,
        2U,
        3U,  // 128-bit data
    };
    std::vector<Ump> packets;
    const std::size_t count =
        for_each_ump(words.data(), words.size(), [&](const Ump& ump) { packets.push_back(ump); });

    ASSERT_EQ(count, 4U);
    ASSERT_EQ(packets.size(), 4U);
    EXPECT_EQ(packets[0].m_size, 1);
    EXPECT_EQ(packets[1].m_size, 2);
    EXPECT_EQ(packets[1].m_words[1], 0x80000000U);
    EXPECT_EQ(packets[2].m_size, 1);
    EXPECT_EQ(packets[3].m_size, 4);
    EXPECT_EQ(packets[3].m_words[3], 3U);
}

// A packet cut off by the end of the buffer must not be delivered half-filled.
TEST(MidiUmp, ForEachUmpDropsATruncatedPacket) {
    const std::array<std::uint32_t, 2> words = {0x20903C64U, 0x40903C00U};
    std::size_t delivered = 0;
    EXPECT_EQ(for_each_ump(words.data(), words.size(), [&](const Ump&) { ++delivered; }), 1U);
    EXPECT_EQ(delivered, 1U);
}

// Android delivers UMP as bytes, each word big-endian.
TEST(MidiUmpByteReader, ReadsBigEndianWords) {
    const std::array<std::uint8_t, 12> bytes = {
        0x20,
        0x90,
        0x3C,
        0x64,  // one-word packet
        0x40,
        0x90,
        0x3C,
        0x00,
        0x80,
        0x00,
        0x12,
        0x34,  // two-word packet
    };
    UmpByteReader reader;
    std::vector<Ump> packets;
    reader.parse(bytes.data(), bytes.size(), [&](const Ump& ump) { packets.push_back(ump); });

    ASSERT_EQ(packets.size(), 2U);
    EXPECT_EQ(packets[0], make_midi1_channel_voice(0, 0x90, 60, 100));
    EXPECT_EQ(packets[1],
              make_midi2_channel_voice(0, ChannelVoiceOpcode::NoteOn, 0, 60, 0, 0x80001234U));
}

TEST(MidiUmpByteReader, PacketsMaySpanCalls) {
    const std::array<std::uint8_t, 8> bytes = {0x40, 0xB1, 0x4A, 0x00, 0xFF, 0xFF, 0xFF, 0xFF};
    UmpByteReader reader;
    std::vector<Ump> packets;
    const auto sink = [&](const Ump& ump) { packets.push_back(ump); };

    reader.parse(bytes.data(), 3, sink);
    EXPECT_TRUE(packets.empty());
    reader.parse(bytes.data() + 3, 3, sink);
    EXPECT_TRUE(packets.empty());
    reader.parse(bytes.data() + 6, 2, sink);

    ASSERT_EQ(packets.size(), 1U);
    EXPECT_EQ(
        packets[0],
        make_midi2_channel_voice(0, ChannelVoiceOpcode::ControlChange, 1, 74, 0, 0xFFFFFFFFU));
}

TEST(MidiUmpByteReader, ResetDropsAPartialPacket) {
    const std::array<std::uint8_t, 4> note = {0x20, 0x90, 0x3C, 0x64};
    UmpByteReader reader;
    std::vector<Ump> packets;
    const auto sink = [&](const Ump& ump) { packets.push_back(ump); };

    reader.parse(note.data(), 2, sink);  // half a word, then the source is reopened
    reader.reset();
    reader.parse(note.data(), note.size(), sink);

    ASSERT_EQ(packets.size(), 1U);
    EXPECT_EQ(packets[0], make_midi1_channel_voice(0, 0x90, 60, 100));
}
