#include <gtest/gtest.h>
#include <tanh/midi/Event.h>
#include <tanh/midi/EventDecoder.h>
#include <tanh/midi/Ump.h>

#include <array>
#include <cstdint>
#include <vector>

using namespace thl::midi;

namespace {

std::vector<Event> decode(EventDecoder& decoder, const Ump& ump, std::uint32_t offset = 0) {
    std::array<Event, 4> storage{};
    EventWriter writer{storage};
    decoder.decode(ump, offset, 7, writer);
    return {writer.events().begin(), writer.events().end()};
}

std::vector<Event> decode(const Ump& ump) {
    EventDecoder decoder;
    return decode(decoder, ump);
}

Ump cc(std::uint8_t channel, std::uint8_t controller, std::uint8_t value) {
    return make_midi1_channel_voice(0,
                                    static_cast<std::uint8_t>(0xB0 | channel),
                                    controller,
                                    value);
}

}  // namespace

TEST(MidiEventDecoder, Midi1NotesCarryKeyChannelAndVelocity) {
    EventDecoder decoder;
    const auto on = decode(decoder, make_midi1_channel_voice(3, 0x95, 60, 127), 11);
    ASSERT_EQ(on.size(), 1U);
    EXPECT_EQ(on[0].m_type, EventType::NoteOn);
    EXPECT_EQ(on[0].m_group, 3);
    EXPECT_EQ(on[0].m_channel, 5);
    EXPECT_EQ(on[0].m_key, 60);
    EXPECT_FLOAT_EQ(on[0].m_value, 1.0f);
    EXPECT_EQ(on[0].m_sample_offset, 11U);
    EXPECT_EQ(on[0].m_source, 7);
    EXPECT_EQ(on[0].m_voice, k_no_voice);

    const auto off = decode(make_midi1_channel_voice(0, 0x85, 60, 64));
    ASSERT_EQ(off.size(), 1U);
    EXPECT_EQ(off[0].m_type, EventType::NoteOff);
    EXPECT_FLOAT_EQ(off[0].m_value, 64.0f / 127.0f);
}

// MIDI 1.0 has no other way to end a note under running status.
TEST(MidiEventDecoder, Midi1NoteOnWithVelocityZeroIsANoteOff) {
    const auto events = decode(make_midi1_channel_voice(0, 0x90, 60, 0));
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].m_type, EventType::NoteOff);
    EXPECT_EQ(events[0].m_key, 60);
}

TEST(MidiEventDecoder, Midi1ChannelMessagesScaleToUnitRange) {
    const auto controller = decode(cc(2, 74, 127));
    ASSERT_EQ(controller.size(), 1U);
    EXPECT_EQ(controller[0].m_type, EventType::Controller);
    EXPECT_EQ(controller[0].m_index, 74);
    EXPECT_EQ(controller[0].m_channel, 2);
    EXPECT_FLOAT_EQ(controller[0].m_value, 1.0f);
    EXPECT_EQ(controller[0].m_data >> 25U, 127U);

    const auto pressure = decode(make_midi1_channel_voice(0, 0xD0, 0));
    ASSERT_EQ(pressure.size(), 1U);
    EXPECT_EQ(pressure[0].m_type, EventType::ChannelPressure);
    EXPECT_FLOAT_EQ(pressure[0].m_value, 0.0f);

    const auto program = decode(make_midi1_channel_voice(0, 0xC0, 12));
    ASSERT_EQ(program.size(), 1U);
    EXPECT_EQ(program[0].m_type, EventType::ProgramChange);
    EXPECT_EQ(program[0].m_index, 12);

    const auto poly = decode(make_midi1_channel_voice(0, 0xA0, 61, 127));
    ASSERT_EQ(poly.size(), 1U);
    EXPECT_EQ(poly[0].m_type, EventType::NoteExpression);
    EXPECT_EQ(poly[0].expression(), Expression::Pressure);
    EXPECT_EQ(poly[0].m_key, 61);
    EXPECT_FLOAT_EQ(poly[0].m_value, 1.0f);
}

// The centre is exactly 0 and both ends are exactly -1 and 1.
TEST(MidiEventDecoder, PitchBendIsSymmetricAroundZero) {
    const auto bend = [](std::uint8_t lsb, std::uint8_t msb) {
        const auto events = decode(make_midi1_channel_voice(0, 0xE0, lsb, msb));
        EXPECT_EQ(events.size(), 1U);
        EXPECT_EQ(events[0].m_type, EventType::PitchBend);
        return events[0].m_value;
    };
    EXPECT_FLOAT_EQ(bend(0x00, 0x40), 0.0f);
    EXPECT_FLOAT_EQ(bend(0x00, 0x00), -1.0f);
    EXPECT_FLOAT_EQ(bend(0x7F, 0x7F), 1.0f);

    const auto bend2 = [](std::uint32_t value) {
        return decode(make_midi2_channel_voice(0, ChannelVoiceOpcode::PitchBend, 0, 0, 0, value))[0]
            .m_value;
    };
    EXPECT_FLOAT_EQ(bend2(0x80000000U), 0.0f);
    EXPECT_FLOAT_EQ(bend2(0x00000000U), -1.0f);
    EXPECT_FLOAT_EQ(bend2(0xFFFFFFFFU), 1.0f);
}

// CC 101 / 100 select, CC 6 and 38 set: one RegisteredController per data entry, and none
// of those control changes shows up as a Controller.
TEST(MidiEventDecoder, Midi1RpnBecomesOneRegisteredController) {
    EventDecoder decoder;
    EXPECT_TRUE(decode(decoder, cc(0, 101, 0)).empty());
    EXPECT_TRUE(decode(decoder, cc(0, 100, 6)).empty());

    const auto msb = decode(decoder, cc(0, 6, 15));
    ASSERT_EQ(msb.size(), 1U);
    EXPECT_EQ(msb[0].m_type, EventType::RegisteredController);
    EXPECT_EQ(msb[0].m_index, 6);
    EXPECT_EQ(msb[0].m_data >> 25U, 15U);

    const auto lsb = decode(decoder, cc(0, 38, 50));
    ASSERT_EQ(lsb.size(), 1U);
    EXPECT_EQ(lsb[0].m_type, EventType::RegisteredController);
    EXPECT_EQ(lsb[0].m_data >> 25U, 15U);
    EXPECT_EQ((lsb[0].m_data >> 18U) & 0x7FU, 50U);
}

TEST(MidiEventDecoder, Midi1NrpnBecomesAnAssignableController) {
    EventDecoder decoder;
    decode(decoder, cc(4, 99, 2));
    decode(decoder, cc(4, 98, 5));
    const auto events = decode(decoder, cc(4, 6, 127));
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].m_type, EventType::AssignableController);
    EXPECT_EQ(events[0].m_index, 2 * 128 + 5);
    EXPECT_EQ(events[0].m_channel, 4);
    // The MSB alone reaches the top: a knob that sends no LSB still gets to 1.
    EXPECT_FLOAT_EQ(events[0].m_value, 1.0f);
    EXPECT_FLOAT_EQ(decode(decoder, cc(4, 38, 127))[0].m_value, 1.0f);
    EXPECT_NEAR(decode(decoder, cc(4, 6, 64))[0].m_value, 64.0f / 127.0f, 1e-6f);
    EXPECT_GT(decode(decoder, cc(4, 38, 100))[0].m_value, 64.0f / 127.0f);
}

// The selection belongs to its channel, and the null parameter ends it.
TEST(MidiEventDecoder, DataEntryWithoutASelectionIsAnOrdinaryController) {
    EventDecoder decoder;
    const auto plain = decode(decoder, cc(0, 6, 10));
    ASSERT_EQ(plain.size(), 1U);
    EXPECT_EQ(plain[0].m_type, EventType::Controller);
    EXPECT_EQ(plain[0].m_index, 6);

    decode(decoder, cc(0, 101, 0));
    decode(decoder, cc(0, 100, 0));
    EXPECT_EQ(decode(decoder, cc(1, 6, 10))[0].m_type, EventType::Controller);
    EXPECT_EQ(decode(decoder, cc(0, 6, 10))[0].m_type, EventType::RegisteredController);

    decode(decoder, cc(0, 101, 127));
    decode(decoder, cc(0, 100, 127));
    EXPECT_EQ(decode(decoder, cc(0, 6, 10))[0].m_type, EventType::Controller);

    decode(decoder, cc(0, 101, 0));
    decode(decoder, cc(0, 100, 0));
    decoder.reset();
    EXPECT_EQ(decode(decoder, cc(0, 38, 10))[0].m_type, EventType::Controller);
}

TEST(MidiEventDecoder, Midi2NotesUseSixteenBitVelocity) {
    const auto on =
        decode(make_midi2_channel_voice(0, ChannelVoiceOpcode::NoteOn, 9, 72, 0, 0xFFFF0000U));
    ASSERT_EQ(on.size(), 1U);
    EXPECT_EQ(on[0].m_type, EventType::NoteOn);
    EXPECT_EQ(on[0].m_key, 72);
    EXPECT_EQ(on[0].m_channel, 9);
    EXPECT_FLOAT_EQ(on[0].m_value, 1.0f);

    // In MIDI 2.0 a note on with velocity 0 is a note on.
    const auto quiet = decode(make_midi2_channel_voice(0, ChannelVoiceOpcode::NoteOn, 0, 72, 0, 0));
    ASSERT_EQ(quiet.size(), 1U);
    EXPECT_EQ(quiet[0].m_type, EventType::NoteOn);

    const auto off =
        decode(make_midi2_channel_voice(0, ChannelVoiceOpcode::NoteOff, 0, 72, 0, 0x80000000U));
    ASSERT_EQ(off.size(), 1U);
    EXPECT_EQ(off[0].m_type, EventType::NoteOff);
    EXPECT_NEAR(off[0].m_value, 0.5f, 1e-4f);
}

// Attribute type 3 is the note's pitch as Q7.9 semitones.
TEST(MidiEventDecoder, Midi2NoteOnWithPitchAttributeAddsAPitchExpression) {
    const std::uint32_t pitch = (61U << 9U) | 256U;  // 61.5 semitones
    const auto events = decode(
        make_midi2_channel_voice(0, ChannelVoiceOpcode::NoteOn, 0, 60, 3, 0x80000000U | pitch));
    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[0].m_type, EventType::NoteOn);
    EXPECT_EQ(events[1].m_type, EventType::NoteExpression);
    EXPECT_EQ(events[1].expression(), Expression::Pitch);
    EXPECT_EQ(events[1].m_key, 60);
    EXPECT_FLOAT_EQ(events[1].m_value, 61.5f);
}

TEST(MidiEventDecoder, Midi2PerNoteMessagesBecomeNoteExpressions) {
    const auto bend = decode(
        make_midi2_channel_voice(0, ChannelVoiceOpcode::PerNotePitchBend, 1, 60, 0, 0xFFFFFFFFU));
    ASSERT_EQ(bend.size(), 1U);
    EXPECT_EQ(bend[0].expression(), Expression::PitchBend);
    EXPECT_EQ(bend[0].m_key, 60);
    EXPECT_FLOAT_EQ(bend[0].m_value, 1.0f);

    const auto pressure = decode(
        make_midi2_channel_voice(0, ChannelVoiceOpcode::PolyPressure, 1, 60, 0, 0xFFFFFFFFU));
    ASSERT_EQ(pressure.size(), 1U);
    EXPECT_EQ(pressure[0].expression(), Expression::Pressure);
    EXPECT_FLOAT_EQ(pressure[0].m_value, 1.0f);

    const auto timbre =
        decode(make_midi2_channel_voice(0,
                                        ChannelVoiceOpcode::RegisteredPerNoteController,
                                        1,
                                        60,
                                        74,
                                        0x80000000U));
    ASSERT_EQ(timbre.size(), 1U);
    EXPECT_EQ(timbre[0].expression(), Expression::Timbre);
    EXPECT_NEAR(timbre[0].m_value, 0.5f, 1e-6f);

    // Registered per-note controller 3: the pitch as Q7.25 semitones.
    const auto pitch =
        decode(make_midi2_channel_voice(0,
                                        ChannelVoiceOpcode::RegisteredPerNoteController,
                                        1,
                                        60,
                                        3,
                                        (62U << 25U) | (1U << 24U)));
    ASSERT_EQ(pitch.size(), 1U);
    EXPECT_EQ(pitch[0].expression(), Expression::Pitch);
    EXPECT_FLOAT_EQ(pitch[0].m_value, 62.5f);

    // The other per-note controllers have no place in an Event yet.
    EXPECT_TRUE(decode(make_midi2_channel_voice(0,
                                                ChannelVoiceOpcode::RegisteredPerNoteController,
                                                1,
                                                60,
                                                7,
                                                0x80000000U))
                    .empty());
    EXPECT_TRUE(decode(make_midi2_channel_voice(0,
                                                ChannelVoiceOpcode::AssignablePerNoteController,
                                                1,
                                                60,
                                                1,
                                                0x80000000U))
                    .empty());
}

TEST(MidiEventDecoder, Midi2ChannelMessagesUseThirtyTwoBits) {
    const auto controller = decode(
        make_midi2_channel_voice(0, ChannelVoiceOpcode::ControlChange, 3, 1, 0, 0xFFFFFFFFU));
    ASSERT_EQ(controller.size(), 1U);
    EXPECT_EQ(controller[0].m_type, EventType::Controller);
    EXPECT_EQ(controller[0].m_index, 1);
    EXPECT_FLOAT_EQ(controller[0].m_value, 1.0f);

    const auto pressure = decode(
        make_midi2_channel_voice(0, ChannelVoiceOpcode::ChannelPressure, 3, 0, 0, 0x40000000U));
    ASSERT_EQ(pressure.size(), 1U);
    EXPECT_EQ(pressure[0].m_type, EventType::ChannelPressure);
    EXPECT_NEAR(pressure[0].m_value, 0.25f, 1e-6f);

    const auto program =
        decode(make_midi2_channel_voice(0, ChannelVoiceOpcode::ProgramChange, 3, 0, 0, 42U << 24U));
    ASSERT_EQ(program.size(), 1U);
    EXPECT_EQ(program[0].m_type, EventType::ProgramChange);
    EXPECT_EQ(program[0].m_index, 42);

    // An RPN is one message: bank and index in the first word, the data in the second.
    const auto rpn = decode(
        make_midi2_channel_voice(0, ChannelVoiceOpcode::RegisteredController, 0, 0, 6, 15U << 25U));
    ASSERT_EQ(rpn.size(), 1U);
    EXPECT_EQ(rpn[0].m_type, EventType::RegisteredController);
    EXPECT_EQ(rpn[0].m_index, 6);
    EXPECT_EQ(rpn[0].m_data >> 25U, 15U);

    const auto nrpn =
        decode(make_midi2_channel_voice(0, ChannelVoiceOpcode::AssignableController, 0, 1, 2, 0U));
    ASSERT_EQ(nrpn.size(), 1U);
    EXPECT_EQ(nrpn[0].m_type, EventType::AssignableController);
    EXPECT_EQ(nrpn[0].m_index, 128 + 2);
}

TEST(MidiEventDecoder, EverythingElseGivesNoEvent) {
    EXPECT_TRUE(decode(make_system(0, 0xF8)).empty());
    EXPECT_TRUE(
        decode(make_sysex7(0, SysEx7Status::Complete, {0x7E, 0x7F, 0x06, 0x01, 0, 0}, 4)).empty());
    EXPECT_TRUE(decode(make_midi2_channel_voice(0,
                                                ChannelVoiceOpcode::RelativeRegisteredController,
                                                0,
                                                0,
                                                0,
                                                1))
                    .empty());
    EXPECT_TRUE(
        decode(make_midi2_channel_voice(0, ChannelVoiceOpcode::PerNoteManagement, 0, 60, 3, 0))
            .empty());
}

TEST(MidiEventWriter, CountsWhatDoesNotFit) {
    std::array<Event, 2> storage{};
    EventWriter writer{storage};
    EXPECT_TRUE(writer.push(Event{}));
    EXPECT_TRUE(writer.push(Event{}));
    EXPECT_FALSE(writer.push(Event{}));
    EXPECT_EQ(writer.size(), 2U);
    EXPECT_EQ(writer.events().size(), 2U);
    EXPECT_EQ(writer.dropped(), 1U);
}

// The LSB refines the MSB before it: every further CC 38 re-uses that MSB.
TEST(MidiEventDecoder, EveryLsbReusesTheMsbBeforeIt) {
    EventDecoder decoder;
    decode(decoder, cc(0, 101, 0));
    decode(decoder, cc(0, 100, 0));
    decode(decoder, cc(0, 6, 12));

    for (const std::uint8_t lsb : {std::uint8_t{10}, std::uint8_t{20}}) {
        const auto events = decode(decoder, cc(0, 38, lsb));
        ASSERT_EQ(events.size(), 1U);
        EXPECT_EQ(events[0].m_data >> 25U, 12U);
        EXPECT_EQ((events[0].m_data >> 18U) & 0x7FU, lsb);
    }

    // A new MSB starts over with an LSB of 0.
    const auto msb = decode(decoder, cc(0, 6, 13));
    ASSERT_EQ(msb.size(), 1U);
    EXPECT_EQ(msb[0].m_data >> 25U, 13U);
    EXPECT_EQ((msb[0].m_data >> 18U) & 0x7FU, 0U);
}

// An LSB with no MSB since its parameter was selected has nothing to refine: not the
// last parameter's MSB either.
TEST(MidiEventDecoder, LsbWithoutAnMsbIsIgnored) {
    EventDecoder decoder;
    decode(decoder, cc(0, 101, 0));
    decode(decoder, cc(0, 100, 0));
    EXPECT_TRUE(decode(decoder, cc(0, 38, 50)).empty());

    decode(decoder, cc(0, 6, 12));
    decode(decoder, cc(0, 100, 6));  // another parameter
    EXPECT_TRUE(decode(decoder, cc(0, 38, 50)).empty());
    EXPECT_EQ(decode(decoder, cc(0, 6, 3))[0].m_index, 6);
}

// Other controllers and other channels between the four messages change nothing.
TEST(MidiEventDecoder, RpnSurvivesUnrelatedMessagesInBetween) {
    EventDecoder decoder;
    decode(decoder, cc(0, 101, 0));
    EXPECT_EQ(decode(decoder, cc(0, 7, 100))[0].m_type, EventType::Controller);
    decode(decoder, cc(0, 100, 0));
    decode(decoder, cc(1, 99, 1));
    decode(decoder, cc(1, 98, 2));
    EXPECT_EQ(decode(decoder, make_midi1_channel_voice(0, 0x90, 60, 100))[0].m_type,
              EventType::NoteOn);

    const auto rpn = decode(decoder, cc(0, 6, 24));
    ASSERT_EQ(rpn.size(), 1U);
    EXPECT_EQ(rpn[0].m_type, EventType::RegisteredController);
    EXPECT_EQ(rpn[0].m_index, 0);

    const auto nrpn = decode(decoder, cc(1, 6, 1));
    ASSERT_EQ(nrpn.size(), 1U);
    EXPECT_EQ(nrpn[0].m_type, EventType::AssignableController);
    EXPECT_EQ(nrpn[0].m_index, 128 + 2);
}
