#include <gtest/gtest.h>
#include <tanh/midi/Event.h>
#include <tanh/midi/Monitor.h>

#include <cstddef>
#include <cstdint>
#include <vector>

using namespace thl::midi;

namespace {

Event make(EventType type, std::uint8_t channel, std::uint16_t index, float value) {
    Event event;
    event.m_type = type;
    event.m_channel = channel;
    event.m_index = index;
    event.m_value = value;
    return event;
}

Event note(bool on, std::uint8_t channel, std::uint8_t key, float velocity) {
    Event event = make(on ? EventType::NoteOn : EventType::NoteOff, channel, 0, velocity);
    event.m_key = key;
    return event;
}

void observe(Monitor& monitor, const std::vector<Event>& events) {
    monitor.observe(events);
}

}  // namespace

TEST(MidiMonitor, KeepsTheLastValueOfEveryController) {
    Monitor monitor;
    EXPECT_FLOAT_EQ(monitor.controller(2, 74), 0.0f);

    observe(monitor,
            {make(EventType::Controller, 2, 74, 0.25f),
             make(EventType::Controller, 2, 74, 0.75f),
             make(EventType::Controller, 3, 74, 0.5f),
             make(EventType::PitchBend, 2, 0, -0.5f),
             make(EventType::ChannelPressure, 2, 0, 0.9f)});

    EXPECT_FLOAT_EQ(monitor.controller(2, 74), 0.75f);
    EXPECT_FLOAT_EQ(monitor.controller(3, 74), 0.5f);
    EXPECT_FLOAT_EQ(monitor.controller(2, 1), 0.0f);
    EXPECT_FLOAT_EQ(monitor.pitch_bend(2), -0.5f);
    EXPECT_FLOAT_EQ(monitor.pitch_bend(3), 0.0f);
    EXPECT_FLOAT_EQ(monitor.channel_pressure(2), 0.9f);
    EXPECT_EQ(monitor.event_count(), 5U);
}

TEST(MidiMonitor, ShowsWhichKeysAreHeld) {
    Monitor monitor;
    observe(monitor, {note(true, 0, 60, 0.5f), note(true, 0, 64, 1.0f), note(true, 5, 60, 0.0f)});
    EXPECT_FLOAT_EQ(monitor.note_velocity(0, 60), 0.5f);
    EXPECT_FLOAT_EQ(monitor.note_velocity(0, 64), 1.0f);
    // Velocity 0 is still a held key.
    EXPECT_GT(monitor.note_velocity(5, 60), 0.0f);
    EXPECT_EQ(monitor.held_notes(), 3U);

    // The same key again is still one key; a note off nobody holds changes nothing.
    observe(monitor, {note(true, 0, 60, 0.7f), note(false, 0, 61, 0.0f)});
    EXPECT_EQ(monitor.held_notes(), 3U);
    EXPECT_FLOAT_EQ(monitor.note_velocity(0, 60), 0.7f);

    observe(monitor, {note(false, 0, 60, 0.0f)});
    EXPECT_FLOAT_EQ(monitor.note_velocity(0, 60), 0.0f);
    EXPECT_EQ(monitor.held_notes(), 2U);
}

TEST(MidiMonitor, AllNotesOffClearsTheChannel) {
    Monitor monitor;
    observe(monitor, {note(true, 0, 60, 0.5f), note(true, 0, 64, 0.5f), note(true, 1, 60, 0.5f)});
    observe(monitor, {make(EventType::Controller, 0, 123, 0.0f)});
    EXPECT_FLOAT_EQ(monitor.note_velocity(0, 60), 0.0f);
    EXPECT_FLOAT_EQ(monitor.note_velocity(0, 64), 0.0f);
    EXPECT_GT(monitor.note_velocity(1, 60), 0.0f);
    EXPECT_EQ(monitor.held_notes(), 1U);
}

// What MIDI learn waits for: which controller moved, and a serial that says it moved.
TEST(MidiMonitor, ReportsTheControllerThatMovedLast) {
    Monitor monitor;
    EXPECT_EQ(monitor.last_controller().m_serial, 0U);

    observe(monitor, {make(EventType::Controller, 4, 21, 0.3f)});
    auto last = monitor.last_controller();
    EXPECT_EQ(last.m_serial, 1U);
    EXPECT_EQ(last.m_type, EventType::Controller);
    EXPECT_EQ(last.m_channel, 4);
    EXPECT_EQ(last.m_index, 21);
    EXPECT_FLOAT_EQ(last.m_value, 0.3f);

    observe(monitor, {make(EventType::PitchBend, 1, 99, 0.5f)});
    last = monitor.last_controller();
    EXPECT_EQ(last.m_serial, 2U);
    EXPECT_EQ(last.m_type, EventType::PitchBend);
    EXPECT_EQ(last.m_channel, 1);
    EXPECT_EQ(last.m_index, 0);

    observe(monitor, {make(EventType::AssignableController, 0, 300, 1.0f)});
    last = monitor.last_controller();
    EXPECT_EQ(last.m_type, EventType::AssignableController);
    EXPECT_EQ(last.m_index, 300);

    observe(monitor, {make(EventType::ChannelPressure, 7, 0, 0.2f)});
    EXPECT_EQ(monitor.last_controller().m_type, EventType::ChannelPressure);
    EXPECT_EQ(monitor.last_controller().m_serial, 4U);
}

// Notes, mode messages and RPNs are nothing a parameter is mapped to.
TEST(MidiMonitor, NotesModeMessagesAndRpnsAreNoControllerMoves) {
    Monitor monitor;
    observe(monitor,
            {note(true, 0, 60, 1.0f),
             make(EventType::Controller, 0, 123, 0.0f),
             make(EventType::Controller, 0, 120, 0.0f),
             make(EventType::RegisteredController, 0, 6, 0.1f),
             make(EventType::ProgramChange, 0, 3, 0.0f)});
    EXPECT_EQ(monitor.last_controller().m_serial, 0U);
}

TEST(MidiMonitor, LogHoldsTheEventsInOrder) {
    Monitor monitor(8);
    observe(monitor,
            {make(EventType::Controller, 0, 1, 0.1f),
             note(true, 0, 60, 1.0f),
             make(EventType::Controller, 0, 1, 0.2f)});

    std::vector<Event> log;
    EXPECT_EQ(monitor.drain([&](const Event& event) { log.push_back(event); }), 3U);
    ASSERT_EQ(log.size(), 3U);
    EXPECT_FLOAT_EQ(log[0].m_value, 0.1f);
    EXPECT_EQ(log[1].m_type, EventType::NoteOn);
    EXPECT_FLOAT_EQ(log[2].m_value, 0.2f);
    EXPECT_EQ(monitor.drain([](const Event&) {}), 0U);
}

// Nobody draining costs log entries, never state.
TEST(MidiMonitor, FullLogDropsEntriesAndKeepsTheState) {
    Monitor monitor(4);
    const std::size_t capacity = monitor.log_capacity();
    std::vector<Event> events;
    for (std::size_t i = 0; i < capacity + 3; ++i) {
        events.push_back(make(EventType::Controller, 0, 7, static_cast<float>(i)));
    }
    observe(monitor, events);

    EXPECT_EQ(monitor.log_dropped(), 3U);
    EXPECT_FLOAT_EQ(monitor.controller(0, 7), static_cast<float>(capacity + 2));
    EXPECT_EQ(monitor.event_count(), capacity + 3);
}

TEST(MidiMonitor, ResetPutsTheStateBackToRest) {
    Monitor monitor;
    observe(monitor,
            {make(EventType::Controller, 0, 7, 0.5f),
             make(EventType::PitchBend, 0, 0, 0.5f),
             make(EventType::ChannelPressure, 0, 0, 0.5f),
             note(true, 0, 60, 1.0f)});
    monitor.reset();
    EXPECT_FLOAT_EQ(monitor.controller(0, 7), 0.0f);
    EXPECT_FLOAT_EQ(monitor.pitch_bend(0), 0.0f);
    EXPECT_FLOAT_EQ(monitor.channel_pressure(0), 0.0f);
    EXPECT_FLOAT_EQ(monitor.note_velocity(0, 60), 0.0f);
    EXPECT_EQ(monitor.held_notes(), 0U);
    EXPECT_EQ(monitor.event_count(), 4U);
}

// The view for per-note expression: what each voice plays. In MPE this is where a member
// channel's bend, pressure and CC 74 show up.
TEST(MidiMonitor, ShowsWhatEachVoicePlays) {
    Monitor monitor;
    EXPECT_FALSE(monitor.voice(3).m_active);
    EXPECT_FLOAT_EQ(monitor.voice(3).m_timbre, 0.5f);

    const auto expression = [](std::int16_t voice, Expression what, float value) {
        Event event = make(EventType::NoteExpression, 2, static_cast<std::uint16_t>(what), value);
        event.m_key = 61;
        event.m_voice = voice;
        return event;
    };
    Event on = note(true, 2, 61, 0.8f);
    on.m_voice = 3;
    observe(monitor,
            {on,
             expression(3, Expression::Tuning, 1.5f),
             expression(3, Expression::Pressure, 0.6f),
             expression(3, Expression::Timbre, 0.9f)});

    Monitor::VoiceView voice = monitor.voice(3);
    EXPECT_TRUE(voice.m_active);
    EXPECT_EQ(voice.m_channel, 2);
    EXPECT_EQ(voice.m_key, 61);
    EXPECT_FLOAT_EQ(voice.m_velocity, 0.8f);
    EXPECT_FLOAT_EQ(voice.m_tuning, 1.5f);
    EXPECT_FLOAT_EQ(voice.m_pressure, 0.6f);
    EXPECT_FLOAT_EQ(voice.m_timbre, 0.9f);
    EXPECT_FALSE(monitor.voice(0).m_active);

    // The note ends; what it played stays readable.
    Event off = note(false, 2, 61, 0.0f);
    off.m_voice = 3;
    observe(monitor, {off});
    voice = monitor.voice(3);
    EXPECT_FALSE(voice.m_active);
    EXPECT_EQ(voice.m_key, 61);

    // Events without a voice, or with one beyond the view, change nothing here.
    Event beyond = note(true, 0, 60, 1.0f);
    beyond.m_voice = static_cast<std::int16_t>(Monitor::k_max_voices);
    observe(monitor, {beyond, note(true, 0, 62, 1.0f)});
    EXPECT_FALSE(monitor.voice(0).m_active);

    monitor.reset();
    EXPECT_FLOAT_EQ(monitor.voice(3).m_tuning, 0.0f);
    EXPECT_FLOAT_EQ(monitor.voice(3).m_timbre, 0.5f);
}
