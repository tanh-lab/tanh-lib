#include <gtest/gtest.h>
#include <tanh/midi/Event.h>
#include <tanh/midi/VoiceAllocator.h>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

using namespace thl::midi;

namespace {

Event make(EventType type, std::uint8_t channel, std::uint8_t key = 0, float value = 0.0f) {
    Event event;
    event.m_type = type;
    event.m_channel = channel;
    event.m_key = key;
    event.m_value = value;
    return event;
}

Event note_on(std::uint8_t channel, std::uint8_t key, float velocity = 0.8f) {
    return make(EventType::NoteOn, channel, key, velocity);
}

Event note_off(std::uint8_t channel, std::uint8_t key) {
    return make(EventType::NoteOff, channel, key);
}

Event bend(std::uint8_t channel, float value) {
    return make(EventType::PitchBend, channel, 0, value);
}

Event pressure(std::uint8_t channel, float value) {
    return make(EventType::ChannelPressure, channel, 0, value);
}

Event controller(std::uint8_t channel, std::uint16_t index, float value) {
    Event event = make(EventType::Controller, channel, 0, value);
    event.m_index = index;
    return event;
}

// An RPN whose data entry MSB is `msb`.
Event rpn(std::uint8_t channel, std::uint16_t index, std::uint32_t msb) {
    Event event = make(EventType::RegisteredController, channel);
    event.m_index = index;
    event.m_data = msb << 25U;
    return event;
}

Event expression(std::uint8_t channel, std::uint8_t key, Expression what, float value) {
    Event event = make(EventType::NoteExpression, channel, key, value);
    event.m_index = static_cast<std::uint16_t>(what);
    return event;
}

struct Rig {
    VoiceAllocator m_allocator;

    explicit Rig(std::uint32_t voices = 4) { m_allocator.set_voice_count(voices); }

    std::vector<Event> send(const Event& event) {
        std::array<Event, 256> storage{};
        EventWriter writer{storage};
        m_allocator.process(event, writer);
        return {writer.events().begin(), writer.events().end()};
    }

    std::vector<Event> begin_block() {
        std::array<Event, 256> storage{};
        EventWriter writer{storage};
        m_allocator.begin_block(writer);
        return {writer.events().begin(), writer.events().end()};
    }

    // The voice a note on was given.
    int play(std::uint8_t channel, std::uint8_t key) {
        return send(note_on(channel, key))[0].m_voice;
    }
};

// The last value of `what` among the events sent to `voice`.
std::optional<float> last(const std::vector<Event>& events, int voice, Expression what) {
    std::optional<float> value;
    for (const Event& event : events) {
        if (event.m_type == EventType::NoteExpression && event.m_voice == voice &&
            event.expression() == what) {
            value = event.m_value;
        }
    }
    return value;
}

std::size_t count(const std::vector<Event>& events, EventType type) {
    std::size_t n = 0;
    for (const Event& event : events) { n += event.m_type == type ? 1 : 0; }
    return n;
}

}  // namespace

// A note on is the note with its voice, then where that voice starts: its three
// expressions.
TEST(MidiVoiceAllocator, NoteOnGetsAVoiceAndItsThreeExpressions) {
    Rig run;
    const auto events = run.send(note_on(0, 60, 0.5f));
    ASSERT_EQ(events.size(), 4U);
    EXPECT_EQ(events[0].m_type, EventType::NoteOn);
    EXPECT_EQ(events[0].m_voice, 0);
    EXPECT_FLOAT_EQ(*last(events, 0, Expression::Tuning), 0.0f);
    EXPECT_FLOAT_EQ(*last(events, 0, Expression::Pressure), 0.0f);
    EXPECT_FLOAT_EQ(*last(events, 0, Expression::Timbre), 0.5f);

    const VoiceAllocator::Voice& voice = run.m_allocator.voice(0);
    EXPECT_TRUE(voice.m_active);
    EXPECT_EQ(voice.m_key, 60);
    EXPECT_FLOAT_EQ(voice.m_velocity, 0.5f);
}

TEST(MidiVoiceAllocator, NoteOffFreesTheVoiceOfItsNote) {
    Rig run;
    EXPECT_EQ(run.play(0, 60), 0);
    EXPECT_EQ(run.play(0, 64), 1);

    const auto off = run.send(note_off(0, 60));
    ASSERT_EQ(off.size(), 1U);
    EXPECT_EQ(off[0].m_type, EventType::NoteOff);
    EXPECT_EQ(off[0].m_voice, 0);
    EXPECT_FALSE(run.m_allocator.voice(0).m_active);
    EXPECT_TRUE(run.m_allocator.voice(1).m_active);

    // A note off nobody holds passes without a voice.
    EXPECT_EQ(run.send(note_off(0, 99))[0].m_voice, k_no_voice);
}

// Free voices are used before any is taken twice, and of the free ones the one released
// longest ago: release tails get the most time.
TEST(MidiVoiceAllocator, FreeVoiceReleasedLongestAgoIsNext) {
    Rig run(3);
    EXPECT_EQ(run.play(0, 60), 0);
    EXPECT_EQ(run.play(0, 61), 1);
    EXPECT_EQ(run.play(0, 62), 2);
    run.send(note_off(0, 61));
    run.send(note_off(0, 60));
    EXPECT_EQ(run.play(0, 70), 1);
    EXPECT_EQ(run.play(0, 71), 0);
}

// With every voice busy the oldest note loses its voice. It gets no note off: the note on
// on a sounding voice is the steal, and its own note off later frees nothing.
TEST(MidiVoiceAllocator, StealsTheOldestNote) {
    Rig run(2);
    EXPECT_EQ(run.play(0, 60), 0);
    EXPECT_EQ(run.play(0, 61), 1);

    const auto stolen = run.send(note_on(0, 62));
    EXPECT_EQ(stolen[0].m_voice, 0);
    EXPECT_EQ(count(stolen, EventType::NoteOff), 0U);
    EXPECT_EQ(run.m_allocator.voice(0).m_key, 62);

    EXPECT_EQ(run.send(note_off(0, 60))[0].m_voice, k_no_voice);
    EXPECT_TRUE(run.m_allocator.voice(0).m_active);
}

TEST(MidiVoiceAllocator, SameKeyStruckAgainKeepsItsVoice) {
    Rig run;
    EXPECT_EQ(run.play(0, 60), 0);
    EXPECT_EQ(run.play(0, 60), 0);
    EXPECT_FALSE(run.m_allocator.voice(1).m_active);
}

// The same key on another channel or from another input is another note.
TEST(MidiVoiceAllocator, NotesAreToldApartByChannelAndInput) {
    Rig run;
    EXPECT_EQ(run.play(0, 60), 0);
    EXPECT_EQ(run.play(1, 60), 1);
    Event other = note_on(0, 60);
    other.m_source = 5;
    EXPECT_EQ(run.send(other)[0].m_voice, 2);

    EXPECT_EQ(run.send(note_off(1, 60))[0].m_voice, 1);
}

// Without MPE a channel's bend moves every note on it by up to 2 semitones, and the bend
// is passed on as the channel event it is.
TEST(MidiVoiceAllocator, ChannelBendTunesTheNotesOfItsChannel) {
    Rig run;
    run.play(0, 60);
    run.play(0, 64);
    run.play(1, 67);

    const auto events = run.send(bend(0, 0.5f));
    EXPECT_EQ(events[0].m_type, EventType::PitchBend);
    EXPECT_FLOAT_EQ(*last(events, 0, Expression::Tuning), 1.0f);
    EXPECT_FLOAT_EQ(*last(events, 1, Expression::Tuning), 1.0f);
    EXPECT_FALSE(last(events, 2, Expression::Tuning).has_value());

    // A note that starts while the wheel is held starts bent.
    EXPECT_FLOAT_EQ(*last(run.send(note_on(0, 72)), 3, Expression::Tuning), 1.0f);
}

TEST(MidiVoiceAllocator, Rpn0SetsTheBendRange) {
    Rig run;
    run.play(0, 60);
    run.send(bend(0, 1.0f));
    const auto events = run.send(rpn(0, 0, 12));
    EXPECT_EQ(events[0].m_type, EventType::RegisteredController);
    EXPECT_FLOAT_EQ(*last(events, 0, Expression::Tuning), 12.0f);
}

// Channel pressure reaches every note of its channel, key pressure one note; a voice
// gets the larger of the two.
TEST(MidiVoiceAllocator, PressureIsTheLargestThatReachesTheNote) {
    Rig run;
    run.play(0, 60);
    run.play(0, 64);

    auto events = run.send(pressure(0, 0.4f));
    EXPECT_EQ(events[0].m_type, EventType::ChannelPressure);
    EXPECT_FLOAT_EQ(*last(events, 0, Expression::Pressure), 0.4f);
    EXPECT_FLOAT_EQ(*last(events, 1, Expression::Pressure), 0.4f);

    events = run.send(expression(0, 64, Expression::Pressure, 0.9f));
    EXPECT_FLOAT_EQ(*last(events, 1, Expression::Pressure), 0.9f);
    EXPECT_FALSE(last(events, 0, Expression::Pressure).has_value());

    // A value that changes nothing for a voice sends nothing.
    events = run.send(expression(0, 64, Expression::Pressure, 0.9f));
    EXPECT_TRUE(events.empty());
}

// MIDI 2.0: the note's own bend, pitch and timbre.
TEST(MidiVoiceAllocator, PerNoteExpressionReachesOnlyItsNote) {
    Rig run;
    run.play(0, 60);
    run.play(0, 64);

    auto events = run.send(expression(0, 64, Expression::PitchBend, 0.5f));
    EXPECT_FLOAT_EQ(*last(events, 1, Expression::Tuning), 24.0f);
    EXPECT_FALSE(last(events, 0, Expression::Tuning).has_value());

    run.m_allocator.set_per_note_bend_range(2.0f);
    events = run.send(expression(0, 64, Expression::PitchBend, 1.0f));
    EXPECT_FLOAT_EQ(*last(events, 1, Expression::Tuning), 2.0f);

    // Pitch 61.5 on key 60: one and a half semitones up.
    events = run.send(expression(0, 60, Expression::Pitch, 61.5f));
    EXPECT_FLOAT_EQ(*last(events, 0, Expression::Tuning), 1.5f);

    events = run.send(expression(0, 60, Expression::Timbre, 0.8f));
    EXPECT_FLOAT_EQ(*last(events, 0, Expression::Timbre), 0.8f);

    // Expression for a note nobody holds is dropped.
    EXPECT_TRUE(run.send(expression(0, 99, Expression::Timbre, 0.8f)).empty());
}

// Outside MPE, CC 74 is a controller like any other.
TEST(MidiVoiceAllocator, Cc74IsAnOrdinaryControllerOutsideMpe) {
    Rig run;
    run.play(0, 60);
    const auto events = run.send(controller(0, 74, 1.0f));
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].m_type, EventType::Controller);
    EXPECT_FLOAT_EQ(run.m_allocator.voice(0).m_timbre, 0.5f);
}

// The MPE Configuration Message: RPN 6 on channel 1 with the number of member channels.
TEST(MidiVoiceAllocator, MpeConfigurationMessageSetsTheZone) {
    Rig run;
    EXPECT_EQ(run.m_allocator.lower_zone_members(), 0);
    run.send(rpn(0, 6, 15));
    EXPECT_EQ(run.m_allocator.lower_zone_members(), 15);
    EXPECT_EQ(run.m_allocator.upper_zone_members(), 0);

    run.send(rpn(0, 6, 0));
    EXPECT_EQ(run.m_allocator.lower_zone_members(), 0);

    // Only on the two manager channels, and only when the allocator follows.
    run.send(rpn(3, 6, 5));
    EXPECT_EQ(run.m_allocator.lower_zone_members(), 0);
    run.m_allocator.set_follow_configuration_messages(false);
    run.send(rpn(0, 6, 7));
    EXPECT_EQ(run.m_allocator.lower_zone_members(), 0);
}

// A channel belongs to one zone: the zone configured last keeps its channels.
TEST(MidiVoiceAllocator, NewerZoneTakesOverlappingChannels) {
    Rig run;
    run.send(rpn(0, 6, 7));
    run.send(rpn(15, 6, 7));
    EXPECT_EQ(run.m_allocator.lower_zone_members(), 7);
    EXPECT_EQ(run.m_allocator.upper_zone_members(), 7);

    run.send(rpn(15, 6, 10));
    EXPECT_EQ(run.m_allocator.upper_zone_members(), 10);
    EXPECT_EQ(run.m_allocator.lower_zone_members(), 4);

    // All fifteen include the other zone's manager channel: that zone is off.
    run.send(rpn(0, 6, 15));
    EXPECT_EQ(run.m_allocator.lower_zone_members(), 15);
    EXPECT_EQ(run.m_allocator.upper_zone_members(), 0);
}

// One note per member channel: its bend, pressure and CC 74 are that note's alone, with
// a bend range of 48 semitones, and are not passed on as channel events.
TEST(MidiVoiceAllocator, MpeMemberChannelMessagesBelongToTheNote) {
    Rig run;
    run.send(rpn(0, 6, 15));
    run.play(1, 60);
    run.play(2, 64);

    auto events = run.send(bend(1, 0.25f));
    EXPECT_EQ(count(events, EventType::PitchBend), 0U);
    EXPECT_FLOAT_EQ(*last(events, 0, Expression::Tuning), 12.0f);
    EXPECT_FALSE(last(events, 1, Expression::Tuning).has_value());

    events = run.send(pressure(2, 0.7f));
    EXPECT_EQ(count(events, EventType::ChannelPressure), 0U);
    EXPECT_FLOAT_EQ(*last(events, 1, Expression::Pressure), 0.7f);

    events = run.send(controller(2, 74, 1.0f));
    EXPECT_EQ(count(events, EventType::Controller), 0U);
    EXPECT_FLOAT_EQ(*last(events, 1, Expression::Timbre), 1.0f);
}

// MPE controllers send a note's bend, pressure and CC 74 before its note on.
TEST(MidiVoiceAllocator, MpeNoteStartsFromItsChannelsState) {
    Rig run;
    run.send(rpn(0, 6, 15));
    run.send(bend(3, -0.5f));
    run.send(pressure(3, 0.3f));
    run.send(controller(3, 74, 0.25f));

    const auto events = run.send(note_on(3, 60));
    EXPECT_FLOAT_EQ(*last(events, 0, Expression::Tuning), -24.0f);
    EXPECT_FLOAT_EQ(*last(events, 0, Expression::Pressure), 0.3f);
    EXPECT_FLOAT_EQ(*last(events, 0, Expression::Timbre), 0.25f);
}

// The manager channel speaks for the zone: its bend (2 semitones) adds to every note's,
// its pressure counts where it is larger, its CC 74 moves every note's around the centre.
TEST(MidiVoiceAllocator, MpeManagerChannelReachesEveryNoteOfItsZone) {
    Rig run;
    run.send(rpn(0, 6, 15));
    run.play(1, 60);
    run.play(2, 64);
    run.send(bend(1, 0.25f));

    auto events = run.send(bend(0, 1.0f));
    EXPECT_EQ(count(events, EventType::PitchBend), 1U);
    EXPECT_FLOAT_EQ(*last(events, 0, Expression::Tuning), 14.0f);
    EXPECT_FLOAT_EQ(*last(events, 1, Expression::Tuning), 2.0f);

    events = run.send(pressure(0, 0.6f));
    EXPECT_FLOAT_EQ(*last(events, 0, Expression::Pressure), 0.6f);
    EXPECT_FLOAT_EQ(*last(events, 1, Expression::Pressure), 0.6f);

    run.send(controller(1, 74, 0.5f));
    events = run.send(controller(0, 74, 0.75f));
    EXPECT_EQ(count(events, EventType::Controller), 1U);
    EXPECT_FLOAT_EQ(*last(events, 0, Expression::Timbre), 0.75f);
}

// RPN 0 on any member channel sets the range of all members; on the manager, its own.
TEST(MidiVoiceAllocator, MpeBendRangesFollowRpn0) {
    Rig run;
    run.send(rpn(0, 6, 15));
    run.play(1, 60);
    run.send(bend(1, 1.0f));
    run.send(bend(0, 1.0f));
    EXPECT_FLOAT_EQ(run.m_allocator.voice(0).m_tuning, 50.0f);

    run.send(rpn(5, 0, 24));
    EXPECT_FLOAT_EQ(run.m_allocator.voice(0).m_tuning, 26.0f);
    run.send(rpn(0, 0, 12));
    EXPECT_FLOAT_EQ(run.m_allocator.voice(0).m_tuning, 36.0f);

    // A new configuration message puts both back.
    run.send(rpn(0, 6, 15));
    EXPECT_FLOAT_EQ(run.m_allocator.voice(0).m_tuning, 50.0f);
}

// The upper zone: manager on channel 16, members downwards from 15.
TEST(MidiVoiceAllocator, UpperZoneCountsDownFromChannel15) {
    Rig run;
    run.send(rpn(15, 6, 3));
    run.play(12, 60);  // member
    run.play(11, 64);  // not a member

    auto events = run.send(bend(12, 0.5f));
    EXPECT_EQ(count(events, EventType::PitchBend), 0U);
    EXPECT_FLOAT_EQ(*last(events, 0, Expression::Tuning), 24.0f);

    events = run.send(bend(11, 0.5f));
    EXPECT_EQ(count(events, EventType::PitchBend), 1U);
    EXPECT_FLOAT_EQ(*last(events, 1, Expression::Tuning), 1.0f);

    events = run.send(bend(15, 0.5f));
    EXPECT_FLOAT_EQ(*last(events, 0, Expression::Tuning), 25.0f);
    EXPECT_FALSE(last(events, 1, Expression::Tuning).has_value());
}

// set_zones() from the interface takes effect at the next block and retunes what sounds.
TEST(MidiVoiceAllocator, ZonesSetByHandApplyAtTheNextBlock) {
    Rig run;
    run.play(1, 60);
    run.send(bend(1, 0.5f));
    EXPECT_FLOAT_EQ(run.m_allocator.voice(0).m_tuning, 1.0f);

    run.m_allocator.set_zones(15, 0);
    EXPECT_EQ(run.m_allocator.lower_zone_members(), 0);
    const auto events = run.begin_block();
    EXPECT_EQ(run.m_allocator.lower_zone_members(), 15);
    EXPECT_FLOAT_EQ(*last(events, 0, Expression::Tuning), 24.0f);

    // Nothing waiting: nothing happens.
    EXPECT_TRUE(run.begin_block().empty());

    // Where the two overlap, the lower zone wins.
    run.m_allocator.set_zones(10, 10);
    run.begin_block();
    EXPECT_EQ(run.m_allocator.lower_zone_members(), 10);
    EXPECT_EQ(run.m_allocator.upper_zone_members(), 4);
}

// All Notes Off and All Sound Off end the notes of their channel; on a manager channel,
// of the zone.
TEST(MidiVoiceAllocator, AllNotesOffReleasesTheChannel) {
    Rig run;
    run.play(0, 60);
    run.play(1, 64);

    auto events = run.send(controller(0, 123, 0.0f));
    EXPECT_EQ(events[0].m_type, EventType::Controller);
    ASSERT_EQ(count(events, EventType::NoteOff), 1U);
    EXPECT_EQ(events[1].m_voice, 0);
    EXPECT_EQ(events[1].m_key, 60);
    EXPECT_FALSE(run.m_allocator.voice(0).m_active);
    EXPECT_TRUE(run.m_allocator.voice(1).m_active);

    run.send(rpn(0, 6, 15));
    run.play(2, 67);
    events = run.send(controller(0, 120, 0.0f));
    EXPECT_EQ(count(events, EventType::NoteOff), 2U);
    EXPECT_FALSE(run.m_allocator.voice(1).m_active);
}

TEST(MidiVoiceAllocator, ResetAllControllersPutsTheChannelBackToRest) {
    Rig run;
    run.play(0, 60);
    run.send(bend(0, 1.0f));
    run.send(pressure(0, 0.8f));

    const auto events = run.send(controller(0, 121, 0.0f));
    EXPECT_FLOAT_EQ(*last(events, 0, Expression::Tuning), 0.0f);
    EXPECT_FLOAT_EQ(*last(events, 0, Expression::Pressure), 0.0f);
}

// Panic: a note off for every voice that has played, also one that is silent here (its
// note off may be the message that got lost downstream), and none for a voice never used.
TEST(MidiVoiceAllocator, ReleaseAllEndsEveryVoiceThatPlayed) {
    Rig run;
    run.play(0, 60);
    run.play(3, 64);
    run.send(note_off(0, 60));

    std::array<Event, 16> storage{};
    EventWriter writer{storage};
    run.m_allocator.release_all(9, writer);
    ASSERT_EQ(writer.size(), 2U);
    EXPECT_EQ(writer.events()[0].m_type, EventType::NoteOff);
    EXPECT_EQ(writer.events()[0].m_voice, 0);
    EXPECT_EQ(writer.events()[0].m_key, 60);
    EXPECT_EQ(writer.events()[1].m_type, EventType::NoteOff);
    EXPECT_EQ(writer.events()[1].m_voice, 1);
    EXPECT_EQ(writer.events()[1].m_key, 64);
    EXPECT_EQ(writer.events()[1].m_channel, 3);
    EXPECT_EQ(writer.events()[1].m_sample_offset, 9U);
    EXPECT_FALSE(run.m_allocator.voice(1).m_active);
}

// What was lost may have been a pedal coming up: after a panic both pedals count as up.
TEST(MidiVoiceAllocator, ReleaseAllLiftsThePedals) {
    Rig run;
    run.play(0, 60);
    run.send(controller(0, 64, 1.0f));
    run.send(controller(0, 66, 1.0f));

    std::array<Event, 16> storage{};
    EventWriter writer{storage};
    run.m_allocator.release_all(0, writer);

    const int voice = run.play(0, 62);
    EXPECT_EQ(run.send(note_off(0, 62))[0].m_voice, voice);
}

// A zone that is alone may have all fifteen member channels: switching the other zone off
// takes nothing from it.
TEST(MidiVoiceAllocator, LoneZoneKeepsAllFifteenMembers) {
    Rig run;
    run.m_allocator.set_zones(0, 15);
    run.begin_block();
    EXPECT_EQ(run.m_allocator.upper_zone_members(), 15);
    EXPECT_EQ(run.m_allocator.lower_zone_members(), 0);

    run.send(rpn(0, 6, 0));
    EXPECT_EQ(run.m_allocator.upper_zone_members(), 15);

    run.m_allocator.set_zones(15, 0);
    run.begin_block();
    run.send(rpn(15, 6, 0));
    EXPECT_EQ(run.m_allocator.lower_zone_members(), 15);
}

// reset() forgets the notes and the controllers but not the zones.
TEST(MidiVoiceAllocator, ResetKeepsTheZones) {
    Rig run;
    run.send(rpn(0, 6, 15));
    run.play(1, 60);
    run.send(bend(1, 1.0f));

    run.m_allocator.reset();
    EXPECT_FALSE(run.m_allocator.voice(0).m_active);
    EXPECT_EQ(run.m_allocator.lower_zone_members(), 15);

    const auto events = run.send(note_on(1, 60));
    EXPECT_FLOAT_EQ(*last(events, 0, Expression::Tuning), 0.0f);
    run.send(bend(1, 1.0f));
    EXPECT_FLOAT_EQ(run.m_allocator.voice(0).m_tuning, 48.0f);
}

TEST(MidiVoiceAllocator, VoiceCountIsClamped) {
    VoiceAllocator allocator;
    allocator.set_voice_count(0);
    EXPECT_EQ(allocator.voice_count(), 1U);
    allocator.set_voice_count(1000);
    EXPECT_EQ(allocator.voice_count(), VoiceAllocator::k_max_voices);
}

// Program changes and the like pass through untouched.
TEST(MidiVoiceAllocator, OtherEventsPassThrough) {
    Rig run;
    const auto events = run.send(make(EventType::ProgramChange, 2));
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].m_type, EventType::ProgramChange);
    EXPECT_EQ(events[0].m_voice, k_no_voice);
}

// ── Pedals ──────────────────────────────────────────────────────────────────

// With the sustain pedal down, the key's note off passes without a voice and the voice
// sounds on; the note off with the voice comes when the pedal comes up.
TEST(MidiVoiceAllocator, SustainPedalHoldsNotesUntilItComesUp) {
    Rig run;
    run.play(0, 60);
    run.play(0, 64);
    auto events = run.send(controller(0, 64, 1.0f));
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].m_type, EventType::Controller);

    events = run.send(note_off(0, 60));
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].m_voice, k_no_voice);
    EXPECT_TRUE(run.m_allocator.voice(0).m_active);
    EXPECT_FALSE(run.m_allocator.voice(0).m_key_down);
    EXPECT_TRUE(run.m_allocator.voice(1).m_key_down);

    // The pedal lets go of the note whose key is up, not of the one still held.
    events = run.send(controller(0, 64, 0.0f));
    ASSERT_EQ(count(events, EventType::NoteOff), 1U);
    EXPECT_EQ(events[1].m_voice, 0);
    EXPECT_EQ(events[1].m_key, 60);
    EXPECT_FALSE(run.m_allocator.voice(0).m_active);
    EXPECT_TRUE(run.m_allocator.voice(1).m_active);

    // Pedal up again: a note off ends its note at once.
    EXPECT_EQ(run.send(note_off(0, 64))[0].m_voice, 1);
}

// 63 is up, 64 is down.
TEST(MidiVoiceAllocator, SustainPedalIsDownFromHalfway) {
    Rig run;
    run.play(0, 60);
    run.send(controller(0, 64, 63.0f / 127.0f));
    EXPECT_EQ(run.send(note_off(0, 60))[0].m_voice, 0);

    run.play(0, 60);
    run.send(controller(0, 64, 64.0f / 127.0f));
    EXPECT_EQ(run.send(note_off(0, 60))[0].m_voice, k_no_voice);
}

// A key struck again while the pedal holds its note takes the same voice and is a held
// key again.
TEST(MidiVoiceAllocator, SustainedKeyStruckAgainIsHeldByItsKey) {
    Rig run;
    run.play(0, 60);
    run.send(controller(0, 64, 1.0f));
    run.send(note_off(0, 60));
    EXPECT_EQ(run.play(0, 60), 0);
    EXPECT_TRUE(run.m_allocator.voice(0).m_key_down);

    EXPECT_EQ(count(run.send(controller(0, 64, 0.0f)), EventType::NoteOff), 0U);
    EXPECT_TRUE(run.m_allocator.voice(0).m_active);
}

// Sostenuto holds the notes whose keys were down when it was pressed, and no others.
TEST(MidiVoiceAllocator, SostenutoHoldsOnlyTheNotesDownWhenPressed) {
    Rig run;
    run.play(0, 60);
    run.send(controller(0, 66, 1.0f));
    run.play(0, 64);
    // The pedal still down takes no new notes.
    run.send(controller(0, 66, 1.0f));

    EXPECT_EQ(run.send(note_off(0, 60))[0].m_voice, k_no_voice);
    EXPECT_EQ(run.send(note_off(0, 64))[0].m_voice, 1);
    EXPECT_TRUE(run.m_allocator.voice(0).m_active);

    const auto events = run.send(controller(0, 66, 0.0f));
    ASSERT_EQ(count(events, EventType::NoteOff), 1U);
    EXPECT_EQ(events[1].m_voice, 0);
}

// A note both pedals hold sounds until both are up.
TEST(MidiVoiceAllocator, NoteHeldByBothPedalsNeedsBothUp) {
    Rig run;
    run.play(0, 60);
    run.send(controller(0, 66, 1.0f));
    run.send(controller(0, 64, 1.0f));
    run.send(note_off(0, 60));

    EXPECT_EQ(count(run.send(controller(0, 66, 0.0f)), EventType::NoteOff), 0U);
    EXPECT_EQ(count(run.send(controller(0, 64, 0.0f)), EventType::NoteOff), 1U);
}

// A pedal holds the notes of its own channel; on an MPE manager channel, of the zone.
TEST(MidiVoiceAllocator, PedalReachesItsChannelOrItsZone) {
    Rig run;
    run.play(0, 60);
    run.play(1, 64);
    run.send(controller(0, 64, 1.0f));
    EXPECT_EQ(run.send(note_off(1, 64))[0].m_voice, 1);
    EXPECT_EQ(run.send(note_off(0, 60))[0].m_voice, k_no_voice);
    run.send(controller(0, 64, 0.0f));

    run.send(rpn(0, 6, 15));
    run.play(1, 64);
    run.play(2, 67);
    run.send(controller(0, 64, 1.0f));
    EXPECT_EQ(run.send(note_off(1, 64))[0].m_voice, k_no_voice);
    EXPECT_EQ(run.send(note_off(2, 67))[0].m_voice, k_no_voice);
    EXPECT_EQ(count(run.send(controller(0, 64, 0.0f)), EventType::NoteOff), 2U);
}

// A voice only a pedal holds goes before one whose key is down, however old that one is.
TEST(MidiVoiceAllocator, StealsPedalHeldVoicesBeforeHeldKeys) {
    Rig run(2);
    run.play(0, 60);
    run.play(0, 61);
    run.send(controller(0, 64, 1.0f));
    run.send(note_off(0, 61));

    EXPECT_EQ(run.play(0, 62), 1);
    EXPECT_TRUE(run.m_allocator.voice(0).m_active);
    EXPECT_EQ(run.m_allocator.voice(0).m_key, 60);
}

TEST(MidiVoiceAllocator, PedalsCanBeIgnored) {
    Rig run;
    run.m_allocator.set_follow_pedals(false);
    run.play(0, 60);
    const auto events = run.send(controller(0, 64, 1.0f));
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].m_type, EventType::Controller);
    EXPECT_EQ(run.send(note_off(0, 60))[0].m_voice, 0);
}

// Reset All Controllers lifts the pedals; All Notes Off ends notes whatever holds them.
TEST(MidiVoiceAllocator, ResetAndAllNotesOffEndPedalHeldNotes) {
    Rig run;
    run.play(0, 60);
    run.send(controller(0, 64, 1.0f));
    run.send(controller(0, 66, 1.0f));
    run.send(note_off(0, 60));
    EXPECT_EQ(count(run.send(controller(0, 121, 0.0f)), EventType::NoteOff), 1U);
    EXPECT_FALSE(run.m_allocator.voice(0).m_active);
    // The pedals are up now.
    const int voice = run.play(0, 60);
    EXPECT_EQ(run.send(note_off(0, 60))[0].m_voice, voice);

    run.play(0, 62);
    run.send(controller(0, 64, 1.0f));
    run.send(note_off(0, 62));
    EXPECT_EQ(count(run.send(controller(0, 123, 0.0f)), EventType::NoteOff), 1U);
}

// Sostenuto takes keys, not sounds: a note only the sustain pedal holds is not captured.
TEST(MidiVoiceAllocator, SostenutoDoesNotCaptureNotesTheSustainPedalHolds) {
    Rig run;
    run.play(0, 60);
    run.send(controller(0, 64, 1.0f));
    run.send(note_off(0, 60));
    run.send(controller(0, 66, 1.0f));

    const auto events = run.send(controller(0, 64, 0.0f));
    EXPECT_EQ(count(events, EventType::NoteOff), 1U);
    EXPECT_FALSE(run.m_allocator.voice(0).m_active);
}

// With nothing held the pedal captures nothing, and it captures on every press.
TEST(MidiVoiceAllocator, SostenutoCapturesOnEveryPress) {
    Rig run;
    run.send(controller(0, 66, 1.0f));
    const int first = run.play(0, 60);
    EXPECT_EQ(run.send(note_off(0, 60))[0].m_voice, first);
    run.send(controller(0, 66, 0.0f));

    const int second = run.play(0, 62);
    run.send(controller(0, 66, 1.0f));
    EXPECT_EQ(run.send(note_off(0, 62))[0].m_voice, k_no_voice);
    const auto events = run.send(controller(0, 66, 0.0f));
    ASSERT_EQ(count(events, EventType::NoteOff), 1U);
    EXPECT_EQ(events[1].m_voice, second);
}

// The sustain pedal coming up leaves what the sostenuto pedal holds.
TEST(MidiVoiceAllocator, ReleasingSustainLeavesSostenutoNotesHeld) {
    Rig run;
    run.play(0, 60);
    run.send(controller(0, 66, 1.0f));
    run.send(controller(0, 64, 1.0f));
    run.play(0, 64);
    run.send(note_off(0, 60));
    run.send(note_off(0, 64));

    const auto events = run.send(controller(0, 64, 0.0f));
    ASSERT_EQ(count(events, EventType::NoteOff), 1U);
    EXPECT_EQ(events[1].m_key, 64);
    EXPECT_TRUE(run.m_allocator.voice(0).m_active);
}

// A captured key struck again is still captured, and leaves no stuck note behind.
TEST(MidiVoiceAllocator, CapturedKeyStruckAgainStaysCaptured) {
    Rig run;
    run.play(0, 60);
    run.send(controller(0, 66, 1.0f));
    run.send(note_off(0, 60));
    EXPECT_EQ(run.play(0, 60), 0);
    EXPECT_EQ(run.send(note_off(0, 60))[0].m_voice, k_no_voice);
    EXPECT_TRUE(run.m_allocator.voice(0).m_active);

    EXPECT_EQ(count(run.send(controller(0, 66, 0.0f)), EventType::NoteOff), 1U);
    for (std::size_t i = 0; i < 4; ++i) { EXPECT_FALSE(run.m_allocator.voice(i).m_active); }
}

// A chord larger than the polyphony, at once or staggered: every note gets one of the
// voices, and when all keys are up no voice is left sounding.
TEST(MidiVoiceAllocator, MoreNotesThanVoicesLeavesNothingSounding) {
    for (const std::uint32_t voices : {1U, 2U, 4U, 16U}) {
        Rig run(voices);
        for (std::uint8_t key = 20; key < 100; ++key) {
            const int voice = run.play(0, key);
            ASSERT_GE(voice, 0);
            ASSERT_LT(voice, static_cast<int>(voices));
        }
        std::size_t freed = 0;
        for (std::uint8_t key = 20; key < 100; ++key) {
            freed += run.send(note_off(0, key))[0].m_voice != k_no_voice ? 1 : 0;
        }
        // Only the notes that still had a voice free one.
        EXPECT_EQ(freed, voices);
        for (std::size_t i = 0; i < voices; ++i) {
            EXPECT_FALSE(run.m_allocator.voice(i).m_active) << voices << " voices, voice " << i;
        }
    }
}

// MPE controllers reuse channels quickly: the same key on two member channels is two
// notes, and each note off ends its own.
TEST(MidiVoiceAllocator, SameKeyOnTwoMemberChannelsIsTwoNotes) {
    Rig run;
    run.send(rpn(0, 6, 15));
    const int first = run.play(1, 60);
    const int second = run.play(2, 60);
    EXPECT_NE(first, second);
    EXPECT_EQ(run.send(note_off(2, 60))[0].m_voice, second);
    EXPECT_TRUE(run.m_allocator.voice(static_cast<std::size_t>(first)).m_active);
    EXPECT_EQ(run.send(note_off(1, 60))[0].m_voice, first);
}

// RPN 0 with a fine part: the MSB is semitones, the LSB cents.
TEST(MidiVoiceAllocator, BendRangeTakesSemitonesAndCents) {
    Rig run;
    run.play(0, 60);
    run.send(bend(0, 1.0f));
    Event range = rpn(0, 0, 2);
    range.m_data |= 50U << 18U;
    run.send(range);
    EXPECT_FLOAT_EQ(run.m_allocator.voice(0).m_tuning, 2.5f);
}
