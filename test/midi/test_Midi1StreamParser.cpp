#include <gtest/gtest.h>
#include <tanh/midi/Midi1StreamParser.h>

#include <cstdint>
#include <initializer_list>
#include <vector>

using namespace thl::midi;

namespace {

std::vector<Ump> parse(Midi1StreamParser& parser, std::initializer_list<std::uint8_t> bytes) {
    std::vector<Ump> packets;
    const std::vector<std::uint8_t> data(bytes);
    parser.parse(data.data(), data.size(), [&](const Ump& ump) { packets.push_back(ump); });
    return packets;
}

std::vector<std::uint8_t> sysex_payload(const std::vector<Ump>& packets) {
    std::vector<std::uint8_t> payload;
    for (const Ump& ump : packets) {
        if (ump.message_type() != MessageType::SysEx7) { continue; }
        for (std::uint8_t i = 0; i < sysex7_byte_count(ump); ++i) {
            payload.push_back(sysex7_byte(ump, i));
        }
    }
    return payload;
}

SysEx7Status sysex_status(const Ump& ump) {
    return static_cast<SysEx7Status>((ump.m_words[0] >> 20U) & 0xFU);
}

}  // namespace

TEST(Midi1StreamParser, ChannelVoiceMessages) {
    Midi1StreamParser parser;
    const auto packets = parse(parser,
                               {0x90,
                                60,
                                100,  // note on
                                0x80,
                                60,
                                64,  // note off
                                0xA1,
                                60,
                                30,  // poly pressure
                                0xB2,
                                74,
                                127,  // control change
                                0xE3,
                                0x00,
                                0x40 /* pitch bend centre */});
    ASSERT_EQ(packets.size(), 5U);
    EXPECT_EQ(packets[0], make_midi1_channel_voice(0, 0x90, 60, 100));
    EXPECT_EQ(packets[1], make_midi1_channel_voice(0, 0x80, 60, 64));
    EXPECT_EQ(packets[2], make_midi1_channel_voice(0, 0xA1, 60, 30));
    EXPECT_EQ(packets[3], make_midi1_channel_voice(0, 0xB2, 74, 127));
    EXPECT_EQ(packets[4], make_midi1_channel_voice(0, 0xE3, 0x00, 0x40));
}

// Program change and channel pressure take a single data byte.
TEST(Midi1StreamParser, OneDataByteMessages) {
    Midi1StreamParser parser;
    const auto packets = parse(parser, {0xC4, 12, 0xD5, 99});
    ASSERT_EQ(packets.size(), 2U);
    EXPECT_EQ(packets[0], make_midi1_channel_voice(0, 0xC4, 12));
    EXPECT_EQ(packets[1], make_midi1_channel_voice(0, 0xD5, 99));
}

// The parser does not interpret: a note-on with velocity 0 stays a note-on.
TEST(Midi1StreamParser, KeepsNoteOnWithVelocityZero) {
    Midi1StreamParser parser;
    const auto packets = parse(parser, {0x90, 60, 0});
    ASSERT_EQ(packets.size(), 1U);
    EXPECT_EQ(packets[0], make_midi1_channel_voice(0, 0x90, 60, 0));
}

TEST(Midi1StreamParser, RunningStatus) {
    Midi1StreamParser parser;
    const auto packets = parse(parser, {0x90, 60, 100, 62, 101, 64, 0});
    ASSERT_EQ(packets.size(), 3U);
    EXPECT_EQ(packets[0], make_midi1_channel_voice(0, 0x90, 60, 100));
    EXPECT_EQ(packets[1], make_midi1_channel_voice(0, 0x90, 62, 101));
    EXPECT_EQ(packets[2], make_midi1_channel_voice(0, 0x90, 64, 0));

    // ... also for one-data-byte messages.
    const auto pressure = parse(parser, {0xD0, 10, 20, 30});
    ASSERT_EQ(pressure.size(), 3U);
    EXPECT_EQ(pressure[2], make_midi1_channel_voice(0, 0xD0, 30));
}

// Android hands over whatever the transport read: a message may end in the next call.
TEST(Midi1StreamParser, MessagesMaySpanCalls) {
    Midi1StreamParser parser;
    EXPECT_TRUE(parse(parser, {0x90}).empty());
    EXPECT_TRUE(parse(parser, {60}).empty());
    const auto packets = parse(parser, {100, 0x80, 60});
    ASSERT_EQ(packets.size(), 1U);
    EXPECT_EQ(packets[0], make_midi1_channel_voice(0, 0x90, 60, 100));

    const auto rest = parse(parser, {0});
    ASSERT_EQ(rest.size(), 1U);
    EXPECT_EQ(rest[0], make_midi1_channel_voice(0, 0x80, 60, 0));
}

// System real-time may sit between the bytes of another message, and must neither be
// delayed nor disturb it.
TEST(Midi1StreamParser, RealTimeInsideAMessage) {
    Midi1StreamParser parser;
    const auto packets = parse(parser, {0x90, 60, 0xF8, 100, 0xFE, 62, 0xFA, 101});
    ASSERT_EQ(packets.size(), 5U);
    EXPECT_EQ(packets[0], make_system(0, 0xF8));
    EXPECT_EQ(packets[1], make_midi1_channel_voice(0, 0x90, 60, 100));
    EXPECT_EQ(packets[2], make_system(0, 0xFE));
    EXPECT_EQ(packets[3], make_system(0, 0xFA));
    EXPECT_EQ(packets[4], make_midi1_channel_voice(0, 0x90, 62, 101));
}

TEST(Midi1StreamParser, SystemCommonMessages) {
    Midi1StreamParser parser;
    const auto packets = parse(parser,
                               {0xF1,
                                0x23,  // time code quarter frame
                                0xF2,
                                0x12,
                                0x34,  // song position
                                0xF3,
                                0x05,  // song select
                                0xF6 /* tune request */});
    ASSERT_EQ(packets.size(), 4U);
    EXPECT_EQ(packets[0], make_system(0, 0xF1, 0x23));
    EXPECT_EQ(packets[1], make_system(0, 0xF2, 0x12, 0x34));
    EXPECT_EQ(packets[2], make_system(0, 0xF3, 0x05));
    EXPECT_EQ(packets[3], make_system(0, 0xF6));
}

// System common messages cancel running status: data bytes after them belong to nothing.
TEST(Midi1StreamParser, SystemCommonCancelsRunningStatus) {
    Midi1StreamParser parser;
    const auto packets = parse(parser, {0x90, 60, 100, 0xF3, 0x05, 62, 101});
    ASSERT_EQ(packets.size(), 2U);
    EXPECT_EQ(packets[0], make_midi1_channel_voice(0, 0x90, 60, 100));
    EXPECT_EQ(packets[1], make_system(0, 0xF3, 0x05));
}

TEST(Midi1StreamParser, IgnoresDataWithoutStatusAndUndefinedStatus) {
    Midi1StreamParser parser;
    EXPECT_TRUE(parse(parser, {60, 100, 0x7F}).empty());
    EXPECT_TRUE(parse(parser, {0xF4, 0xF5, 0xF9, 0xFD, 0xF7}).empty());

    // An undefined system common status still cancels the message in progress.
    const auto packets = parse(parser, {0x90, 60, 0xF4, 100, 0x90, 61, 99});
    ASSERT_EQ(packets.size(), 1U);
    EXPECT_EQ(packets[0], make_midi1_channel_voice(0, 0x90, 61, 99));
}

// A new status byte abandons a message that is still short of data bytes.
TEST(Midi1StreamParser, NewStatusAbandonsIncompleteMessage) {
    Midi1StreamParser parser;
    const auto packets = parse(parser, {0x90, 60, 0xB0, 7, 100});
    ASSERT_EQ(packets.size(), 1U);
    EXPECT_EQ(packets[0], make_midi1_channel_voice(0, 0xB0, 7, 100));
}

TEST(Midi1StreamParser, StampsTheGroup) {
    Midi1StreamParser parser(9);
    EXPECT_EQ(parser.group(), 9);
    const auto packets = parse(parser, {0x90, 60, 100, 0xF8, 0xF0, 1, 0xF7});
    ASSERT_EQ(packets.size(), 3U);
    for (const Ump& ump : packets) { EXPECT_EQ(ump.group(), 9); }
}

TEST(Midi1StreamParser, ShortSysExIsOneCompletePacket) {
    Midi1StreamParser parser;
    const auto packets = parse(parser, {0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7});
    ASSERT_EQ(packets.size(), 1U);
    EXPECT_EQ(sysex_status(packets[0]), SysEx7Status::Complete);
    EXPECT_EQ(sysex_payload(packets), (std::vector<std::uint8_t>{0x7E, 0x7F, 0x06, 0x01}));
}

TEST(Midi1StreamParser, EmptySysEx) {
    Midi1StreamParser parser;
    const auto packets = parse(parser, {0xF0, 0xF7});
    ASSERT_EQ(packets.size(), 1U);
    EXPECT_EQ(sysex_status(packets[0]), SysEx7Status::Complete);
    EXPECT_EQ(sysex7_byte_count(packets[0]), 0);
}

// Exactly six bytes fit one packet: it must come out Complete, not Start plus an empty End.
TEST(Midi1StreamParser, SixByteSysExIsOneCompletePacket) {
    Midi1StreamParser parser;
    const auto packets = parse(parser, {0xF0, 1, 2, 3, 4, 5, 6, 0xF7});
    ASSERT_EQ(packets.size(), 1U);
    EXPECT_EQ(sysex_status(packets[0]), SysEx7Status::Complete);
    EXPECT_EQ(sysex_payload(packets), (std::vector<std::uint8_t>{1, 2, 3, 4, 5, 6}));
}

TEST(Midi1StreamParser, LongSysExIsChunked) {
    Midi1StreamParser parser;
    const auto packets = parse(parser, {0xF0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 0xF7});
    ASSERT_EQ(packets.size(), 3U);
    EXPECT_EQ(sysex_status(packets[0]), SysEx7Status::Start);
    EXPECT_EQ(sysex_status(packets[1]), SysEx7Status::Continue);
    EXPECT_EQ(sysex_status(packets[2]), SysEx7Status::End);
    EXPECT_EQ(sysex7_byte_count(packets[2]), 2);
    EXPECT_EQ(sysex_payload(packets),
              (std::vector<std::uint8_t>{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14}));

    // Twelve bytes: Start and a full End, no Continue.
    const auto twelve = parse(parser, {0xF0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 0xF7});
    ASSERT_EQ(twelve.size(), 2U);
    EXPECT_EQ(sysex_status(twelve[0]), SysEx7Status::Start);
    EXPECT_EQ(sysex_status(twelve[1]), SysEx7Status::End);
    EXPECT_EQ(sysex7_byte_count(twelve[1]), 6);
}

TEST(Midi1StreamParser, RealTimeInsideSysEx) {
    Midi1StreamParser parser;
    const auto packets = parse(parser, {0xF0, 1, 2, 0xF8, 3, 0xF7});
    ASSERT_EQ(packets.size(), 2U);
    EXPECT_EQ(packets[0], make_system(0, 0xF8));
    EXPECT_EQ(sysex_status(packets[1]), SysEx7Status::Complete);
    EXPECT_EQ(sysex_payload(packets), (std::vector<std::uint8_t>{1, 2, 3}));
}

// Any status byte ends a System Exclusive, and still starts its own message.
TEST(Midi1StreamParser, StatusByteEndsSysEx) {
    Midi1StreamParser parser;
    const auto packets = parse(parser, {0xF0, 1, 2, 3, 0x90, 60, 100});
    ASSERT_EQ(packets.size(), 2U);
    EXPECT_EQ(sysex_status(packets[0]), SysEx7Status::Complete);
    EXPECT_EQ(sysex_payload(packets), (std::vector<std::uint8_t>{1, 2, 3}));
    EXPECT_EQ(packets[1], make_midi1_channel_voice(0, 0x90, 60, 100));
}

// The one byte that completes two packets: a tune request cutting a System Exclusive short.
TEST(Midi1StreamParser, TuneRequestEndingSysExYieldsBoth) {
    Midi1StreamParser parser;
    Midi1StreamParser::Output out;
    for (const std::uint8_t byte : {0xF0, 1, 2}) {
        EXPECT_EQ(parser.feed(static_cast<std::uint8_t>(byte), out), 0);
    }
    ASSERT_EQ(parser.feed(0xF6, out), 2);
    EXPECT_EQ(out[0].message_type(), MessageType::SysEx7);
    EXPECT_EQ(sysex_status(out[0]), SysEx7Status::Complete);
    EXPECT_EQ(out[1], make_system(0, 0xF6));
}

// A second 0xF0 ends the open System Exclusive and starts a new one.
TEST(Midi1StreamParser, SysExStartEndsOpenSysEx) {
    Midi1StreamParser parser;
    const auto packets = parse(parser, {0xF0, 1, 2, 0xF0, 3, 0xF7});
    ASSERT_EQ(packets.size(), 2U);
    EXPECT_EQ(sysex_status(packets[0]), SysEx7Status::Complete);
    EXPECT_EQ(sysex7_byte_count(packets[0]), 2);
    EXPECT_EQ(sysex_status(packets[1]), SysEx7Status::Complete);
    EXPECT_EQ(sysex7_byte(packets[1], 0), 3);
}

TEST(Midi1StreamParser, ResetForgetsEverything) {
    Midi1StreamParser parser;
    EXPECT_TRUE(parse(parser, {0x90, 60}).empty());
    parser.reset();
    EXPECT_TRUE(parse(parser, {100, 61, 101}).empty());  // no running status left

    EXPECT_TRUE(parse(parser, {0xF0, 1, 2, 3, 4, 5, 6, 7}).size() == 1U);  // Start went out
    parser.reset();
    const auto packets = parse(parser, {0xF0, 9, 0xF7});
    ASSERT_EQ(packets.size(), 1U);
    EXPECT_EQ(sysex_status(packets[0]), SysEx7Status::Complete);  // not an End of the old one
    EXPECT_EQ(sysex_payload(packets), (std::vector<std::uint8_t>{9}));
}
