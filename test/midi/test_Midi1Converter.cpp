#include <gtest/gtest.h>
#include <tanh/midi/Midi1Converter.h>
#include <tanh/midi/Midi1StreamParser.h>

#include <array>
#include <cstdint>
#include <vector>

using namespace thl::midi;

namespace {

std::vector<std::uint8_t> to_bytes(const Ump& ump) {
    Midi1Messages messages;
    const std::size_t count = convert_to_midi1(ump, messages);
    std::vector<std::uint8_t> bytes;
    for (std::size_t i = 0; i < count; ++i) {
        bytes.insert(bytes.end(),
                     messages[i].m_bytes.begin(),
                     messages[i].m_bytes.begin() + messages[i].m_size);
    }
    return bytes;
}

using Bytes = std::vector<std::uint8_t>;

}  // namespace

// MIDI 1.0 in, MIDI 1.0 out: bytes that went through the parser come back unchanged.
TEST(Midi1Converter, RoundTripsAMidi1Stream) {
    const Bytes stream = {
        0x90, 60,   100,   // note on
        0x80, 60,   0,     // note off
        0xA1, 60,   30,    // poly pressure
        0xB2, 74,   127,   // control change
        0xC3, 12,          // program change
        0xD4, 99,          // channel pressure
        0xE5, 0x7F, 0x7F,  // pitch bend
        0xF1, 0x23,        // time code
        0xF2, 0x12, 0x34,  // song position
        0xF3, 0x05,        // song select
        0xF6,              // tune request
        0xF8, 0xFA, 0xFB, 0xFC, 0xFE, 0xFF,
    };

    Midi1StreamParser parser;
    Bytes result;
    parser.parse(stream.data(), stream.size(), [&](const Ump& ump) {
        const Bytes bytes = to_bytes(ump);
        result.insert(result.end(), bytes.begin(), bytes.end());
    });
    EXPECT_EQ(result, stream);
}

TEST(Midi1Converter, Midi2NoteScalesVelocity) {
    // 16-bit velocity 0xC000 -> 7-bit 0x60.
    EXPECT_EQ(
        to_bytes(make_midi2_channel_voice(0, ChannelVoiceOpcode::NoteOn, 3, 60, 0, 0xC0000000U)),
        (Bytes{0x93, 60, 0x60}));
    EXPECT_EQ(
        to_bytes(make_midi2_channel_voice(0, ChannelVoiceOpcode::NoteOff, 3, 60, 0, 0x80000000U)),
        (Bytes{0x83, 60, 0x40}));
    // Full scale stays full scale.
    EXPECT_EQ(
        to_bytes(make_midi2_channel_voice(0, ChannelVoiceOpcode::NoteOn, 0, 127, 0, 0xFFFF0000U)),
        (Bytes{0x90, 127, 127}));
}

// A quiet MIDI 2.0 note-on must not turn into a MIDI 1.0 note-off.
TEST(Midi1Converter, Midi2NoteOnNeverGetsVelocityZero) {
    EXPECT_EQ(
        to_bytes(make_midi2_channel_voice(0, ChannelVoiceOpcode::NoteOn, 0, 60, 0, 0x00010000U)),
        (Bytes{0x90, 60, 1}));
    EXPECT_EQ(to_bytes(make_midi2_channel_voice(0, ChannelVoiceOpcode::NoteOn, 0, 60, 0, 0U)),
              (Bytes{0x90, 60, 1}));
    // A note-off may carry velocity 0.
    EXPECT_EQ(to_bytes(make_midi2_channel_voice(0, ChannelVoiceOpcode::NoteOff, 0, 60, 0, 0U)),
              (Bytes{0x80, 60, 0}));
}

TEST(Midi1Converter, Midi2ControllersAndPressure) {
    EXPECT_EQ(
        to_bytes(
            make_midi2_channel_voice(0, ChannelVoiceOpcode::ControlChange, 1, 74, 0, 0xFFFFFFFFU)),
        (Bytes{0xB1, 74, 127}));
    EXPECT_EQ(
        to_bytes(
            make_midi2_channel_voice(0, ChannelVoiceOpcode::ControlChange, 1, 1, 0, 0x80000000U)),
        (Bytes{0xB1, 1, 64}));
    EXPECT_EQ(
        to_bytes(
            make_midi2_channel_voice(0, ChannelVoiceOpcode::ChannelPressure, 2, 0, 0, 0x40000000U)),
        (Bytes{0xD2, 32}));
    EXPECT_EQ(
        to_bytes(
            make_midi2_channel_voice(0, ChannelVoiceOpcode::PolyPressure, 2, 60, 0, 0x20000000U)),
        (Bytes{0xA2, 60, 16}));
}

TEST(Midi1Converter, Midi2PitchBendKeeps14Bits) {
    // Centre, minimum, maximum.
    EXPECT_EQ(
        to_bytes(make_midi2_channel_voice(0, ChannelVoiceOpcode::PitchBend, 0, 0, 0, 0x80000000U)),
        (Bytes{0xE0, 0x00, 0x40}));
    EXPECT_EQ(to_bytes(make_midi2_channel_voice(0, ChannelVoiceOpcode::PitchBend, 0, 0, 0, 0U)),
              (Bytes{0xE0, 0x00, 0x00}));
    EXPECT_EQ(
        to_bytes(make_midi2_channel_voice(0, ChannelVoiceOpcode::PitchBend, 0, 0, 0, 0xFFFFFFFFU)),
        (Bytes{0xE0, 0x7F, 0x7F}));
    // 14-bit value 0x1234 = MSB 0x24, LSB 0x34.
    EXPECT_EQ(
        to_bytes(
            make_midi2_channel_voice(0, ChannelVoiceOpcode::PitchBend, 7, 0, 0, 0x1234U << 18U)),
        (Bytes{0xE7, 0x34, 0x24}));
}

TEST(Midi1Converter, Midi2ProgramChange) {
    // Without a valid bank: the program change alone.
    EXPECT_EQ(
        to_bytes(
            make_midi2_channel_voice(0, ChannelVoiceOpcode::ProgramChange, 4, 0, 0, 0x2A000102U)),
        (Bytes{0xC4, 42}));
    // With the bank-valid flag: bank select MSB, LSB, then the program change.
    EXPECT_EQ(
        to_bytes(
            make_midi2_channel_voice(0, ChannelVoiceOpcode::ProgramChange, 4, 0, 1, 0x2A000102U)),
        (Bytes{0xB4, 0, 1, 0xB4, 32, 2, 0xC4, 42}));
}

TEST(Midi1Converter, Midi2RegisteredControllerBecomesRpnSequence) {
    // RPN 0 (pitch bend sensitivity) = 48 semitones: 14-bit value 48 << 7.
    const std::uint32_t value = (48U << 7U) << 18U;
    EXPECT_EQ(
        to_bytes(
            make_midi2_channel_voice(0, ChannelVoiceOpcode::RegisteredController, 1, 0, 0, value)),
        (Bytes{0xB1, 101, 0, 0xB1, 100, 0, 0xB1, 6, 48, 0xB1, 38, 0}));
    // NRPN 0x12/0x34 with a 14-bit value of MSB 0x56, LSB 0x78.
    const std::uint32_t nrpn = ((0x56U << 7U) | 0x78U) << 18U;
    EXPECT_EQ(to_bytes(make_midi2_channel_voice(0,
                                                ChannelVoiceOpcode::AssignableController,
                                                2,
                                                0x12,
                                                0x34,
                                                nrpn)),
              (Bytes{0xB2, 99, 0x12, 0xB2, 98, 0x34, 0xB2, 6, 0x56, 0xB2, 38, 0x78}));
}

// What MIDI 1.0 cannot say is left out rather than approximated.
TEST(Midi1Converter, DropsWhatMidi1CannotExpress) {
    for (const ChannelVoiceOpcode opcode : {ChannelVoiceOpcode::RegisteredPerNoteController,
                                            ChannelVoiceOpcode::AssignablePerNoteController,
                                            ChannelVoiceOpcode::RelativeRegisteredController,
                                            ChannelVoiceOpcode::RelativeAssignableController,
                                            ChannelVoiceOpcode::PerNotePitchBend,
                                            ChannelVoiceOpcode::PerNoteManagement}) {
        EXPECT_TRUE(to_bytes(make_midi2_channel_voice(0, opcode, 0, 60, 0, 0x80000000U)).empty());
    }

    const std::array<std::uint8_t, 6> payload = {1, 2, 3, 0, 0, 0};
    EXPECT_TRUE(to_bytes(make_sysex7(0, SysEx7Status::Complete, payload, 3)).empty());

    Ump utility;
    utility.m_words[0] = 0x00200000U;  // jitter-reduction timestamp
    utility.m_size = 1;
    EXPECT_TRUE(to_bytes(utility).empty());

    Ump stream;
    stream.m_words[0] = 0xF0000000U;
    stream.m_size = 4;
    EXPECT_TRUE(to_bytes(stream).empty());
}

// System messages come back at their own length: one, two or three bytes.
TEST(Midi1Converter, SystemMessagesKeepTheirLength) {
    EXPECT_EQ(to_bytes(make_system(0, 0xF1, 0x35)), (Bytes{0xF1, 0x35}));              // time code
    EXPECT_EQ(to_bytes(make_system(0, 0xF2, 0x10, 0x20)), (Bytes{0xF2, 0x10, 0x20}));  // position
    EXPECT_EQ(to_bytes(make_system(0, 0xF3, 0x07)), (Bytes{0xF3, 0x07}));  // song select
    EXPECT_EQ(to_bytes(make_system(0, 0xF6)), (Bytes{0xF6}));              // tune request
    EXPECT_EQ(to_bytes(make_system(0, 0xF8)), (Bytes{0xF8}));              // timing clock
}

// A status that is no short message gives nothing: System Exclusive start and end (their
// data travels as type 3) and the statuses MIDI 1.0 leaves undefined.
TEST(Midi1Converter, StatusesWithoutAShortMessageGiveNothing) {
    for (const int status : {0xF0, 0xF4, 0xF5, 0xF7, 0xF9, 0xFD}) {
        EXPECT_TRUE(to_bytes(make_system(0, static_cast<std::uint8_t>(status))).empty())
            << "status " << status;
    }

    // A data byte where the status belongs: a malformed packet.
    Ump no_status;
    no_status.m_words[0] = 0x20003C40U;
    no_status.m_size = 1;
    EXPECT_TRUE(to_bytes(no_status).empty());
}

// Packets a sender may emit that MIDI 2.0 leaves reserved, or that carry no channel voice
// or system message, give nothing either.
TEST(Midi1Converter, ReservedAndDataPacketsGiveNothing) {
    Ump reserved_opcode;  // MIDI 2.0 channel voice, opcode 0x7
    reserved_opcode.m_words[0] = 0x40703C00U;
    reserved_opcode.m_words[1] = 0x80000000U;
    reserved_opcode.m_size = 2;
    EXPECT_TRUE(to_bytes(reserved_opcode).empty());

    // 0x5 data, 0xD flex data, the rest reserved.
    for (const std::uint32_t type : {0x5U, 0x6U, 0x7U, 0x8U, 0x9U, 0xAU, 0xBU, 0xCU, 0xDU, 0xEU}) {
        Ump ump;
        ump.m_words[0] = type << 28U;
        ump.m_size = ump_word_count(ump.m_words[0]);
        EXPECT_TRUE(to_bytes(ump).empty()) << "message type " << type;
    }
}
