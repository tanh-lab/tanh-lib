#include <gtest/gtest.h>
#include <tanh/midi/AudioMidiProcessor.h>
#include <tanh/midi/Event.h>
#include <tanh/midi/EventStream.h>
#include <tanh/midi/Ump.h>

#include <cstddef>
#include <cstdint>
#include <vector>

using namespace thl::midi;
using thl::BlockEvent;

namespace {

constexpr std::size_t k_block = 128;

BlockEvent at(std::uint32_t offset, const Ump& ump, std::uint16_t source = 0) {
    BlockEvent event;
    event.m_midi = ump;
    event.m_sample_offset = offset;
    event.m_source = source;
    return event;
}

Ump note_on(std::uint8_t channel, std::uint8_t key) {
    return make_midi1_channel_voice(0, static_cast<std::uint8_t>(0x90 | channel), key, 100);
}

Ump note_off(std::uint8_t channel, std::uint8_t key) {
    return make_midi1_channel_voice(0, static_cast<std::uint8_t>(0x80 | channel), key, 0);
}

std::size_t count(const EventStream& stream, EventType type) {
    std::size_t n = 0;
    for (const Event& event : stream.events()) { n += event.m_type == type ? 1 : 0; }
    return n;
}

}  // namespace

// Packets in, events with voices out, each at its packet's sample.
TEST(MidiEventStream, BlockOfPacketsBecomesEventsWithVoices) {
    EventStream stream;
    stream.prepare();

    const std::vector<BlockEvent> block = {
        at(3, note_on(0, 60), 4),
        at(3, note_on(0, 64), 4),
        at(40, make_midi1_channel_voice(0, 0xB0, 1, 127), 4),
        at(90, note_off(0, 60), 4),
    };
    stream.process(block, k_block);

    const auto events = stream.events();
    ASSERT_EQ(events.size(), 10U);  // two notes with three expressions each, CC, note off
    EXPECT_EQ(events[0].m_type, EventType::NoteOn);
    EXPECT_EQ(events[0].m_voice, 0);
    EXPECT_EQ(events[0].m_sample_offset, 3U);
    EXPECT_EQ(events[0].m_source, 4);
    EXPECT_EQ(events[4].m_type, EventType::NoteOn);
    EXPECT_EQ(events[4].m_voice, 1);
    EXPECT_EQ(events[8].m_type, EventType::Controller);
    EXPECT_EQ(events[8].m_sample_offset, 40U);
    EXPECT_EQ(events[9].m_type, EventType::NoteOff);
    EXPECT_EQ(events[9].m_voice, 0);
    EXPECT_EQ(events[9].m_sample_offset, 90U);

    for (std::size_t i = 1; i < events.size(); ++i) {
        EXPECT_LE(events[i - 1].m_sample_offset, events[i].m_sample_offset);
    }
}

// Every call replaces the list; the notes carry over.
TEST(MidiEventStream, ListHoldsOneBlockAndNotesCarryOver) {
    EventStream stream;
    stream.prepare();

    const std::vector<BlockEvent> first = {at(0, note_on(0, 60))};
    stream.process(first, k_block);
    EXPECT_EQ(stream.events().size(), 4U);

    stream.process({}, k_block);
    EXPECT_TRUE(stream.events().empty());
    EXPECT_TRUE(stream.voices().voice(0).m_active);

    const std::vector<BlockEvent> last = {at(5, note_off(0, 60))};
    stream.process(last, k_block);
    ASSERT_EQ(stream.events().size(), 1U);
    EXPECT_EQ(stream.events()[0].m_voice, 0);
}

// An RPN is four control changes that may arrive in different blocks.
TEST(MidiEventStream, RpnStateCarriesAcrossBlocks) {
    EventStream stream;
    stream.prepare();

    const std::vector<BlockEvent> select = {at(0, make_midi1_channel_voice(0, 0xB0, 101, 0)),
                                            at(0, make_midi1_channel_voice(0, 0xB0, 100, 6))};
    stream.process(select, k_block);
    EXPECT_TRUE(stream.events().empty());

    const std::vector<BlockEvent> data = {at(0, make_midi1_channel_voice(0, 0xB0, 6, 15))};
    stream.process(data, k_block);
    EXPECT_EQ(count(stream, EventType::RegisteredController), 1U);
    EXPECT_EQ(stream.voices().lower_zone_members(), 15);
}

TEST(MidiEventStream, VoiceCountComesFromTheConfig) {
    EventStream stream;
    stream.prepare({.m_max_events = 64, .m_voice_count = 2});
    EXPECT_EQ(stream.voices().voice_count(), 2U);

    const std::vector<BlockEvent> block = {at(0, note_on(0, 60)),
                                           at(1, note_on(0, 61)),
                                           at(2, note_on(0, 62))};
    stream.process(block, k_block);
    EXPECT_EQ(stream.events()[8].m_type, EventType::NoteOn);
    EXPECT_EQ(stream.events()[8].m_voice, 0);  // the oldest note's voice
}

// A full list drops what is left and says so; release_all() then ends what may hang.
TEST(MidiEventStream, CountsEventsThatDoNotFit) {
    EventStream stream;
    stream.prepare({.m_max_events = 6, .m_voice_count = 4});

    const std::vector<BlockEvent> block = {at(0, note_on(0, 60)), at(1, note_on(0, 61))};
    stream.process(block, k_block);
    EXPECT_EQ(stream.events().size(), 6U);
    EXPECT_EQ(stream.dropped(), 2U);

    stream.prepare({.m_max_events = 6, .m_voice_count = 4});
    EXPECT_EQ(stream.dropped(), 0U);
}

// The panic button: every sounding note ends at the start of the next block, once.
TEST(MidiEventStream, ReleaseAllEndsNotesAtTheNextBlock) {
    EventStream stream;
    stream.prepare();
    const std::vector<BlockEvent> block = {at(0, note_on(0, 60)), at(0, note_on(0, 64))};
    stream.process(block, k_block);

    stream.release_all();
    stream.process({}, k_block);
    ASSERT_EQ(stream.events().size(), 2U);
    EXPECT_EQ(stream.events()[0].m_type, EventType::NoteOff);
    EXPECT_EQ(stream.events()[0].m_sample_offset, 0U);
    EXPECT_FALSE(stream.voices().voice(0).m_active);
    EXPECT_FALSE(stream.voices().voice(1).m_active);

    stream.process({}, k_block);
    EXPECT_TRUE(stream.events().empty());
}

// Zones set from the interface show up in the block that follows.
TEST(MidiEventStream, ZoneRequestIsAppliedAtBlockStart) {
    EventStream stream;
    stream.prepare();
    const std::vector<BlockEvent> block = {at(0, note_on(1, 60)),
                                           at(0, make_midi1_channel_voice(0, 0xE1, 0x7F, 0x7F))};
    stream.process(block, k_block);
    EXPECT_FLOAT_EQ(stream.voices().voice(0).m_tuning, 2.0f);

    stream.voices().set_zones(15, 0);
    stream.process({}, k_block);
    ASSERT_EQ(stream.events().size(), 1U);
    EXPECT_EQ(stream.events()[0].expression(), Expression::Tuning);
    EXPECT_FLOAT_EQ(stream.events()[0].m_value, 48.0f);
}

// A note off that did not fit the list leaves its voice sounding downstream although the
// allocator has freed it. release_all() ends it all the same.
TEST(MidiEventStream, ReleaseAllEndsANoteWhoseNoteOffWasDropped) {
    EventStream stream;
    stream.prepare({.m_max_events = 5, .m_voice_count = 4});

    const std::vector<BlockEvent> on = {at(0, note_on(0, 60))};
    stream.process(on, k_block);
    // Four events of a new note fill the list; the note off of the first is the fifth... and
    // the sixth does not fit.
    const std::vector<BlockEvent> full = {at(0, note_on(0, 62)),
                                          at(1, make_midi1_channel_voice(0, 0xB0, 1, 1)),
                                          at(2, note_off(0, 60))};
    stream.process(full, k_block);
    EXPECT_EQ(stream.dropped(), 1U);
    EXPECT_EQ(count(stream, EventType::NoteOff), 0U);
    EXPECT_FALSE(stream.voices().voice(0).m_active);

    stream.release_all();
    stream.process({}, k_block);
    EXPECT_EQ(count(stream, EventType::NoteOff), 2U);
    EXPECT_EQ(stream.events()[0].m_voice, 0);
}

TEST(MidiEventStream, RemembersTheBlockLength) {
    EventStream stream;
    stream.prepare();
    EXPECT_EQ(stream.num_samples(), 0U);
    stream.process({}, 48);
    EXPECT_EQ(stream.num_samples(), 48U);
}
