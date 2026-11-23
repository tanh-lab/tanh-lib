#include <gtest/gtest.h>
#include <tanh/midi/AudioMidiProcessor.h>
#include <tanh/midi/Event.h>
#include <tanh/midi/EventStream.h>
#include <tanh/midi/Ump.h>
#include <tanh/modulation/MidiSources.h>
#include <tanh/modulation/ModulationMatrix.h>
#include <tanh/modulation/ModulationRouting.h>
#include <tanh/modulation/ParameterBackend.h>
#include <tanh/modulation/SmartHandle.h>
#include <tanh/parameters/ModulationScope.h>
#include <tanh/parameters/ParameterDefinitions.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace thl::modulation;
using thl::BlockEvent;
using thl::midi::EventStream;
using thl::midi::EventType;
using thl::midi::Ump;

namespace {

constexpr double k_sr = 48000.0;
constexpr size_t k_bs = 64;

/// A ParameterBackend over plain atomics, all parameters in [0, 1], in any scope.
class FakeBackend : public ParameterBackend {
public:
    void add(const std::string& key, float default_value, ModulationScope scope = k_global_scope) {
        auto e = std::make_unique<Entry>();
        e->m_def =
            thl::ParameterDefinition::make_float(key, thl::Range::linear(0.0f, 1.0f), default_value)
                .modulatable(true)
                .modulation_scope(scope);
        e->m_value.store(default_value);
        m_entries[key] = std::move(e);
    }
    std::optional<ParameterBinding> find(std::string_view key) const override {
        auto it = m_entries.find(key);
        if (it == m_entries.end()) { return std::nullopt; }
        return ParameterBinding{.m_def = it->second->m_def, .m_base = &it->second->m_value};
    }

private:
    struct Entry {
        thl::ParameterDefinition m_def;
        std::atomic<float> m_value{0.0f};
    };
    std::map<std::string, std::unique_ptr<Entry>, std::less<>> m_entries;
};

BlockEvent at(std::uint32_t offset, const Ump& ump) {
    BlockEvent event;
    event.m_midi = ump;
    event.m_sample_offset = offset;
    return event;
}

Ump cc(std::uint8_t channel, std::uint8_t controller, std::uint8_t value) {
    return thl::midi::make_midi1_channel_voice(0,
                                               static_cast<std::uint8_t>(0xB0 | channel),
                                               controller,
                                               value);
}

Ump note_on(std::uint8_t channel, std::uint8_t key, std::uint8_t velocity = 127) {
    return thl::midi::make_midi1_channel_voice(0,
                                               static_cast<std::uint8_t>(0x90 | channel),
                                               key,
                                               velocity);
}

Ump note_off(std::uint8_t channel, std::uint8_t key) {
    return thl::midi::make_midi1_channel_voice(0,
                                               static_cast<std::uint8_t>(0x80 | channel),
                                               key,
                                               0);
}

// Pitch bend all the way up / at rest / all the way down.
Ump bend(std::uint8_t channel, int direction) {
    const std::uint8_t status = static_cast<std::uint8_t>(0xE0 | channel);
    if (direction > 0) { return thl::midi::make_midi1_channel_voice(0, status, 0x7F, 0x7F); }
    if (direction < 0) { return thl::midi::make_midi1_channel_voice(0, status, 0x00, 0x00); }
    return thl::midi::make_midi1_channel_voice(0, status, 0x00, 0x40);
}

MidiControllerAddress controller_address(std::uint16_t index, std::int8_t channel = -1) {
    return {.m_type = EventType::Controller, .m_index = index, .m_channel = channel};
}

/// A stream and one block of packets for it.
struct Midi {
    EventStream m_stream;

    explicit Midi(std::uint32_t voices = 4) {
        m_stream.prepare({.m_max_events = 256, .m_voice_count = voices});
    }
    void block(const std::vector<BlockEvent>& packets = {}, size_t num_samples = k_bs) {
        m_stream.process(packets, num_samples);
    }
};

constexpr ModulationScope k_voice_scope{.m_id = 1, .m_name = "voice"};

std::vector<uint32_t> change_points(const ModulationSource& source, uint32_t voice) {
    return source.get_voice_change_points(voice);
}

}  // namespace

// ── MidiControllerSource ────────────────────────────────────────────────────

// The output steps at each event's sample and holds; one change point per step.
TEST(MidiControllerSource, StepsAtTheEventsSampleAndHolds) {
    Midi midi;
    MidiControllerSource source(midi.m_stream, controller_address(74));
    source.prepare(k_sr, k_bs, 1);

    midi.block({at(10, cc(0, 74, 127)), at(30, cc(0, 74, 0))});
    source.clear_per_block();
    source.pre_process_block();

    EXPECT_FLOAT_EQ(source.get_output_at(9), 0.0f);
    EXPECT_FLOAT_EQ(source.get_output_at(10), 1.0f);
    EXPECT_FLOAT_EQ(source.get_output_at(29), 1.0f);
    EXPECT_FLOAT_EQ(source.get_output_at(30), 0.0f);
    EXPECT_EQ(source.get_change_points(), (std::vector<uint32_t>{10, 30}));
    EXPECT_FLOAT_EQ(source.value(), 0.0f);

    midi.block({at(0, cc(0, 74, 64))});
    source.clear_per_block();
    source.pre_process_block();
    midi.block();
    source.clear_per_block();
    source.pre_process_block();
    EXPECT_FLOAT_EQ(source.get_output_at(0), 64.0f / 127.0f);
    EXPECT_FLOAT_EQ(source.get_output_at(k_bs - 1), 64.0f / 127.0f);
    EXPECT_TRUE(source.get_change_points().empty());
    EXPECT_FLOAT_EQ(source.last_output(), 64.0f / 127.0f);
}

// A controller nobody has touched says nothing about where its knob stands.
TEST(MidiControllerSource, IsInactiveUntilTheControllerMoves) {
    Midi midi;
    MidiControllerSource source(midi.m_stream, controller_address(1));
    source.prepare(k_sr, k_bs, 1);

    midi.block();
    source.pre_process_block();
    EXPECT_EQ(source.get_output_active_at(0), 0);
    EXPECT_EQ(source.get_output_active_at(k_bs - 1), 0);

    midi.block({at(20, cc(0, 1, 100))});
    source.pre_process_block();
    EXPECT_EQ(source.get_output_active_at(19), 0);
    EXPECT_EQ(source.get_output_active_at(20), 1);

    midi.block();
    source.pre_process_block();
    EXPECT_EQ(source.get_output_active_at(0), 1);
}

TEST(MidiControllerSource, FollowsOnlyItsAddress) {
    Midi midi;
    MidiControllerSource any(midi.m_stream, controller_address(7));
    MidiControllerSource on_three(midi.m_stream, controller_address(7, 3));
    any.prepare(k_sr, k_bs, 1);
    on_three.prepare(k_sr, k_bs, 1);

    midi.block({at(0, cc(0, 7, 127)), at(1, cc(0, 8, 127)), at(2, bend(0, 1))});
    any.pre_process_block();
    on_three.pre_process_block();
    EXPECT_FLOAT_EQ(any.get_output_at(k_bs - 1), 1.0f);
    EXPECT_EQ(on_three.get_output_active_at(k_bs - 1), 0);

    midi.block({at(0, cc(3, 7, 0))});
    any.pre_process_block();
    on_three.pre_process_block();
    EXPECT_FLOAT_EQ(any.get_output_at(k_bs - 1), 0.0f);
    EXPECT_EQ(on_three.get_output_active_at(k_bs - 1), 1);
}

// Unipolar puts the wheel's rest at 0.5, bipolar at 0.
TEST(MidiControllerSource, PitchBendAndPressureInBothPolarities) {
    Midi midi;
    const MidiControllerAddress wheel{.m_type = EventType::PitchBend};
    const MidiControllerAddress touch{.m_type = EventType::ChannelPressure};
    MidiControllerSource unipolar(midi.m_stream, wheel);
    MidiControllerSource bipolar(midi.m_stream, wheel, MidiPolarity::Bipolar);
    MidiControllerSource pressure(midi.m_stream, touch);
    MidiControllerSource pressure_bipolar(midi.m_stream, touch, MidiPolarity::Bipolar);
    for (auto* source : {&unipolar, &bipolar, &pressure, &pressure_bipolar}) {
        source->prepare(k_sr, k_bs, 1);
    }

    midi.block({at(0, bend(0, 0)),
                at(5, thl::midi::make_midi1_channel_voice(0, 0xD0, 127)),
                at(10, bend(0, 1)),
                at(20, bend(0, -1))});
    for (auto* source : {&unipolar, &bipolar, &pressure, &pressure_bipolar}) {
        source->pre_process_block();
    }
    EXPECT_FLOAT_EQ(unipolar.get_output_at(0), 0.5f);
    EXPECT_FLOAT_EQ(unipolar.get_output_at(10), 1.0f);
    EXPECT_FLOAT_EQ(unipolar.get_output_at(20), 0.0f);
    EXPECT_FLOAT_EQ(bipolar.get_output_at(0), 0.0f);
    EXPECT_FLOAT_EQ(bipolar.get_output_at(10), 1.0f);
    EXPECT_FLOAT_EQ(bipolar.get_output_at(20), -1.0f);
    EXPECT_FLOAT_EQ(pressure.get_output_at(5), 1.0f);
    EXPECT_FLOAT_EQ(pressure_bipolar.get_output_at(5), 1.0f);
    EXPECT_EQ(pressure.get_output_active_at(4), 0);
}

TEST(MidiControllerSource, FollowsAnNrpn) {
    Midi midi;
    MidiControllerSource source(midi.m_stream,
                                {.m_type = EventType::AssignableController, .m_index = 128 + 5});
    source.prepare(k_sr, k_bs, 1);

    midi.block({at(0, cc(0, 99, 1)), at(0, cc(0, 98, 5)), at(8, cc(0, 6, 127))});
    source.pre_process_block();
    EXPECT_EQ(source.get_output_active_at(7), 0);
    EXPECT_FLOAT_EQ(source.get_output_at(8), 1.0f);
}

// MIDI learn re-binds a source: the old controller's value does not carry over.
TEST(MidiControllerSource, NewAddressStartsInactive) {
    Midi midi;
    MidiControllerSource source(midi.m_stream, controller_address(7));
    source.prepare(k_sr, k_bs, 1);
    midi.block({at(0, cc(0, 7, 127))});
    source.pre_process_block();
    EXPECT_EQ(source.get_output_active_at(0), 1);

    source.set_address(controller_address(21, 2));
    EXPECT_EQ(source.address(), controller_address(21, 2));
    midi.block();
    source.clear_per_block();
    source.pre_process_block();
    EXPECT_EQ(source.get_output_active_at(0), 0);
    EXPECT_FLOAT_EQ(source.get_output_at(0), 0.0f);
    EXPECT_EQ(source.get_change_points(), (std::vector<uint32_t>{0}));

    midi.block({at(4, cc(2, 21, 127))});
    source.pre_process_block();
    EXPECT_FLOAT_EQ(source.get_output_at(4), 1.0f);
}

// Through the matrix: a Replace routing leaves the parameter alone until the controller
// moves, then follows it sample by sample.
TEST(MidiControllerSource, DrivesAParameterThroughTheMatrix) {
    FakeBackend backend;
    backend.add("cutoff", 0.4f);
    ModulationMatrix matrix(backend);
    Midi midi;
    MidiControllerSource source(midi.m_stream, controller_address(74));
    matrix.add_source("midi.cc.74", &source);
    auto cutoff = matrix.get_smart_handle<float>("cutoff");
    ModulationRouting routing("midi.cc.74", "cutoff", 1.0f);
    routing.m_combine_mode = CombineMode::Replace;
    matrix.add_routing(routing);
    matrix.prepare(k_sr, k_bs);

    midi.block();
    matrix.process(k_bs);
    for (uint32_t i = 0; i < k_bs; ++i) { EXPECT_FLOAT_EQ(cutoff.load(i), 0.4f); }

    midi.block({at(20, cc(0, 74, 127))});
    matrix.process(k_bs);
    EXPECT_FLOAT_EQ(cutoff.load(19), 0.4f);
    EXPECT_FLOAT_EQ(cutoff.load(20), 1.0f);
    EXPECT_FLOAT_EQ(cutoff.load(k_bs - 1), 1.0f);

    midi.block();
    matrix.process(k_bs);
    EXPECT_FLOAT_EQ(cutoff.load(0), 1.0f);
    matrix.remove_source("midi.cc.74");
}

// ── MidiNoteSource ──────────────────────────────────────────────────────────

// The gate of a voice is open from its note on to its note off, to the sample.
TEST(MidiNoteSource, GateFollowsTheNoteOfItsVoice) {
    Midi midi;
    MidiNoteSource gate(midi.m_stream, MidiNoteStream::Gate, k_voice_scope);
    gate.prepare(k_sr, k_bs, 4);
    EXPECT_EQ(gate.stream(), MidiNoteStream::Gate);

    midi.block({at(5, note_on(0, 60)), at(9, note_on(0, 64)), at(40, note_off(0, 60))});
    gate.clear_per_block();
    gate.pre_process_block();

    EXPECT_FLOAT_EQ(gate.voice_output(0)[4], 0.0f);
    EXPECT_FLOAT_EQ(gate.voice_output(0)[5], 1.0f);
    EXPECT_FLOAT_EQ(gate.voice_output(0)[39], 1.0f);
    EXPECT_FLOAT_EQ(gate.voice_output(0)[40], 0.0f);
    EXPECT_EQ(gate.voice_output_active(0)[5], 1);
    EXPECT_EQ(gate.voice_output_active(0)[40], 0);
    EXPECT_EQ(change_points(gate, 0), (std::vector<uint32_t>{5, 40}));

    EXPECT_FLOAT_EQ(gate.voice_output(1)[8], 0.0f);
    EXPECT_FLOAT_EQ(gate.voice_output(1)[9], 1.0f);
    EXPECT_FLOAT_EQ(gate.voice_output(1)[k_bs - 1], 1.0f);
    EXPECT_FLOAT_EQ(gate.voice_output(2)[k_bs - 1], 0.0f);

    // Held notes stay open over the next blocks.
    midi.block();
    gate.clear_per_block();
    gate.pre_process_block();
    EXPECT_FLOAT_EQ(gate.voice_output(1)[0], 1.0f);
    EXPECT_EQ(gate.voice_output_active(1)[0], 1);
    EXPECT_TRUE(change_points(gate, 1).empty());
}

// Key plus tuning, mapped to 0 to 1 over the pitch range; the last pitch holds after the
// note ended.
TEST(MidiNoteSource, PitchIsTheKeyPlusItsTuning) {
    Midi midi;
    MidiNoteSource pitch(midi.m_stream, MidiNoteStream::Pitch, k_voice_scope);
    pitch.set_pitch_range(48.0f, 72.0f);
    pitch.prepare(k_sr, k_bs, 4);

    midi.block({at(0, note_on(0, 60)), at(10, bend(0, 1)), at(20, note_off(0, 60))});
    pitch.pre_process_block();
    EXPECT_FLOAT_EQ(pitch.voice_output(0)[0], 0.5f);
    EXPECT_FLOAT_EQ(pitch.voice_output(0)[10], 14.0f / 24.0f);  // bent up 2 semitones
    EXPECT_EQ(pitch.voice_output_active(0)[19], 1);
    EXPECT_EQ(pitch.voice_output_active(0)[20], 0);
    EXPECT_FLOAT_EQ(pitch.voice_output(0)[k_bs - 1], 14.0f / 24.0f);

    // Out of range clamps; a note that starts while the wheel is up starts bent.
    midi.block({at(0, note_on(0, 100)), at(1, note_on(0, 20)), at(2, note_on(0, 70))});
    pitch.pre_process_block();
    EXPECT_FLOAT_EQ(pitch.voice_output(1)[0], 1.0f);
    EXPECT_FLOAT_EQ(pitch.voice_output(2)[1], 0.0f);
    EXPECT_FLOAT_EQ(pitch.voice_output(3)[2], 1.0f);  // 70 + 2 = 72

    // An empty range gives 0 instead of dividing by it.
    pitch.set_pitch_range(60.0f, 60.0f);
    midi.block({at(0, note_on(1, 60))});
    pitch.pre_process_block();
    EXPECT_FLOAT_EQ(pitch.voice_output(0)[0], 0.0f);
}

TEST(MidiNoteSource, VelocityPressureAndTimbreReachTheirVoice) {
    Midi midi;
    MidiNoteSource velocity(midi.m_stream, MidiNoteStream::Velocity, k_voice_scope);
    MidiNoteSource pressure(midi.m_stream, MidiNoteStream::Pressure, k_voice_scope);
    MidiNoteSource timbre(midi.m_stream, MidiNoteStream::Timbre, k_voice_scope);
    for (auto* source : {&velocity, &pressure, &timbre}) { source->prepare(k_sr, k_bs, 4); }

    // An MPE controller: zone of 15, then one note per member channel.
    midi.m_stream.voices().set_zones(15, 0);
    midi.block({at(0, note_on(1, 60, 127)),
                at(0, note_on(2, 64, 64)),
                at(10, thl::midi::make_midi1_channel_voice(0, 0xD1, 127)),
                at(20, cc(2, 74, 127))});
    for (auto* source : {&velocity, &pressure, &timbre}) { source->pre_process_block(); }

    EXPECT_FLOAT_EQ(velocity.voice_output(0)[0], 1.0f);
    EXPECT_FLOAT_EQ(velocity.voice_output(1)[0], 64.0f / 127.0f);

    EXPECT_FLOAT_EQ(pressure.voice_output(0)[9], 0.0f);
    EXPECT_FLOAT_EQ(pressure.voice_output(0)[10], 1.0f);
    EXPECT_FLOAT_EQ(pressure.voice_output(1)[10], 0.0f);

    EXPECT_FLOAT_EQ(timbre.voice_output(0)[20], 0.5f);
    EXPECT_FLOAT_EQ(timbre.voice_output(1)[19], 0.5f);
    EXPECT_FLOAT_EQ(timbre.voice_output(1)[20], 1.0f);
    EXPECT_EQ(timbre.voice_output_active(1)[0], 1);
}

// A note on for a voice that still sounds closes the gate for one sample, so an envelope
// behind it starts again; the pitch goes straight to the new note.
TEST(MidiNoteSource, StolenVoiceRetriggersItsGate) {
    Midi midi(1);
    MidiNoteSource gate(midi.m_stream, MidiNoteStream::Gate, k_voice_scope);
    MidiNoteSource pitch(midi.m_stream, MidiNoteStream::Pitch, k_voice_scope);
    gate.prepare(k_sr, k_bs, 1);
    pitch.prepare(k_sr, k_bs, 1);

    midi.block({at(0, note_on(0, 60)), at(10, note_on(0, 72))});
    gate.clear_per_block();
    gate.pre_process_block();
    pitch.pre_process_block();

    EXPECT_FLOAT_EQ(gate.voice_output(0)[9], 1.0f);
    EXPECT_FLOAT_EQ(gate.voice_output(0)[10], 0.0f);
    EXPECT_EQ(gate.voice_output_active(0)[10], 0);
    EXPECT_FLOAT_EQ(gate.voice_output(0)[11], 1.0f);
    EXPECT_EQ(gate.voice_output_active(0)[11], 1);
    EXPECT_EQ(change_points(gate, 0), (std::vector<uint32_t>{0, 10, 11}));

    EXPECT_FLOAT_EQ(pitch.voice_output(0)[9], 60.0f / 127.0f);
    EXPECT_FLOAT_EQ(pitch.voice_output(0)[10], 72.0f / 127.0f);
    EXPECT_EQ(pitch.voice_output_active(0)[10], 1);
}

// On the block's last sample the gate opens again on the first sample of the next block.
TEST(MidiNoteSource, RetriggerOnTheLastSampleReopensInTheNextBlock) {
    Midi midi(1);
    MidiNoteSource gate(midi.m_stream, MidiNoteStream::Gate, k_voice_scope);
    gate.prepare(k_sr, k_bs, 1);

    const auto last = static_cast<std::uint32_t>(k_bs - 1);
    midi.block({at(0, note_on(0, 60)), at(last, note_on(0, 72))});
    gate.pre_process_block();
    EXPECT_FLOAT_EQ(gate.voice_output(0)[last - 1], 1.0f);
    EXPECT_FLOAT_EQ(gate.voice_output(0)[last], 0.0f);

    midi.block();
    gate.clear_per_block();
    gate.pre_process_block();
    EXPECT_FLOAT_EQ(gate.voice_output(0)[0], 1.0f);
    EXPECT_EQ(gate.voice_output_active(0)[0], 1);
    EXPECT_EQ(change_points(gate, 0), (std::vector<uint32_t>{0}));

    // A note off before the gate could reopen keeps it shut.
    midi.block({at(0, note_on(0, 60)), at(last, note_on(0, 72))});
    gate.pre_process_block();
    midi.block({at(0, note_off(0, 72))});
    gate.pre_process_block();
    EXPECT_FLOAT_EQ(gate.voice_output(0)[k_bs - 1], 0.0f);
    midi.block();
    gate.pre_process_block();
    EXPECT_FLOAT_EQ(gate.voice_output(0)[0], 0.0f);
}

// A scope smaller than the stream's voice count ignores the voices it does not have.
TEST(MidiNoteSource, IgnoresVoicesBeyondItsScope) {
    Midi midi(4);
    MidiNoteSource gate(midi.m_stream, MidiNoteStream::Gate, k_voice_scope);
    gate.prepare(k_sr, k_bs, 2);

    midi.block({at(0, note_on(0, 60)), at(0, note_on(0, 61)), at(0, note_on(0, 62))});
    gate.pre_process_block();
    EXPECT_FLOAT_EQ(gate.voice_output(0)[0], 1.0f);
    EXPECT_FLOAT_EQ(gate.voice_output(1)[0], 1.0f);
}

// Through the matrix: each voice's pressure drives that voice's copy of the parameter,
// and a voice without a note keeps the base value.
TEST(MidiNoteSource, DrivesAVoiceParameterThroughTheMatrix) {
    FakeBackend backend;
    ModulationMatrix matrix(backend);
    const ModulationScope voices = matrix.register_scope("voice", 4);
    backend.add("cutoff", 0.4f, voices);

    Midi midi(4);
    MidiNoteSource pressure(midi.m_stream, MidiNoteStream::Pressure, voices);
    matrix.add_source("midi.note.pressure", &pressure);
    auto cutoff = matrix.get_smart_handle<float>("cutoff");
    ModulationRouting routing("midi.note.pressure", "cutoff", 1.0f);
    routing.m_combine_mode = CombineMode::Replace;
    matrix.add_routing(routing);
    matrix.prepare(k_sr, k_bs);

    midi.block({at(0, note_on(0, 60)),
                at(0, note_on(0, 64)),
                at(16, thl::midi::make_midi1_channel_voice(0, 0xA0, 64, 127))});
    matrix.process(k_bs);

    EXPECT_FLOAT_EQ(cutoff.load(0, 0), 0.0f);
    EXPECT_FLOAT_EQ(cutoff.load(k_bs - 1, 0), 0.0f);
    EXPECT_FLOAT_EQ(cutoff.load(15, 1), 0.0f);
    EXPECT_FLOAT_EQ(cutoff.load(16, 1), 1.0f);
    EXPECT_FLOAT_EQ(cutoff.load(0, 2), 0.4f);

    midi.block({at(8, note_off(0, 64))});
    matrix.process(k_bs);
    EXPECT_FLOAT_EQ(cutoff.load(7, 1), 1.0f);
    EXPECT_FLOAT_EQ(cutoff.load(8, 1), 0.4f);
    matrix.remove_source("midi.note.pressure");
}

// However many events a block brings, a sample is a change point once: the lists were
// reserved for one block and must not grow on the audio thread.
TEST(MidiNoteSource, ChangePointsNeverOutgrowTheBlock) {
    constexpr size_t k_small_block = 4;
    Midi midi(1);
    MidiNoteSource gate(midi.m_stream, MidiNoteStream::Gate, k_voice_scope);
    MidiControllerSource wheel(midi.m_stream, controller_address(1));
    gate.prepare(k_sr, k_small_block, 1);
    wheel.prepare(k_sr, k_small_block, 1);

    std::vector<BlockEvent> packets;
    for (std::uint32_t i = 0; i < 40; ++i) {
        const std::uint32_t offset = i / 10;
        packets.push_back(at(offset, note_on(0, static_cast<std::uint8_t>(40 + i))));
        packets.push_back(at(offset, cc(0, 1, static_cast<std::uint8_t>(i))));
    }
    midi.block(packets);
    gate.clear_per_block();
    wheel.clear_per_block();
    gate.pre_process_block();
    wheel.pre_process_block();

    EXPECT_EQ(change_points(gate, 0), (std::vector<uint32_t>{0, 1, 2, 3}));
    EXPECT_EQ(wheel.get_change_points(), (std::vector<uint32_t>{0, 1, 2, 3}));
    EXPECT_FLOAT_EQ(wheel.get_output_at(3), 39.0f / 127.0f);
}

// ── MidiControllerSource: smoothing and soft takeover ───────────────────────

namespace {

// One block for `source` with `packets`.
void run_block(Midi& midi, MidiControllerSource& source, const std::vector<BlockEvent>& packets) {
    midi.block(packets);
    source.clear_per_block();
    source.pre_process_block();
}

}  // namespace

// With smoothing the output walks to a new value in a straight line over the given time,
// across block boundaries, and lands on it exactly. The first value is taken at once.
TEST(MidiControllerSource, SmoothingGlidesToTheNewValue) {
    Midi midi;
    MidiControllerSource source(midi.m_stream, controller_address(1));
    source.set_smoothing_ms(2.0f);  // 96 samples at 48 kHz
    EXPECT_FLOAT_EQ(source.smoothing_ms(), 2.0f);
    source.prepare(k_sr, k_bs, 1);

    run_block(midi, source, {at(10, cc(0, 1, 0))});
    EXPECT_EQ(source.get_output_active_at(9), 0);
    EXPECT_FLOAT_EQ(source.get_output_at(10), 0.0f);

    run_block(midi, source, {at(0, cc(0, 1, 127))});
    EXPECT_NEAR(source.get_output_at(0), 1.0f / 96.0f, 1e-6f);
    EXPECT_NEAR(source.get_output_at(47), 48.0f / 96.0f, 1e-5f);
    EXPECT_NEAR(source.get_output_at(63), 64.0f / 96.0f, 1e-5f);
    for (uint32_t i = 1; i < k_bs; ++i) {
        EXPECT_GT(source.get_output_at(i), source.get_output_at(i - 1));
    }
    // A change point where the glide starts and at least every 32 samples while it runs.
    EXPECT_EQ(source.get_change_points(), (std::vector<uint32_t>{0, 31, 63}));

    run_block(midi, source, {});
    EXPECT_NEAR(source.get_output_at(30), 95.0f / 96.0f, 1e-5f);
    EXPECT_FLOAT_EQ(source.get_output_at(31), 1.0f);
    EXPECT_FLOAT_EQ(source.get_output_at(k_bs - 1), 1.0f);
    EXPECT_EQ(source.get_change_points(), (std::vector<uint32_t>{31}));

    run_block(midi, source, {});
    EXPECT_TRUE(source.get_change_points().empty());
}

// A value that arrives during a glide is aimed at from where the output is.
TEST(MidiControllerSource, NewValueDuringAGlideStartsFromTheCurrentOutput) {
    Midi midi;
    MidiControllerSource source(midi.m_stream, controller_address(1));
    source.set_smoothing_ms(2.0f);
    source.prepare(k_sr, k_bs, 1);
    run_block(midi, source, {at(0, cc(0, 1, 0))});

    run_block(midi, source, {at(0, cc(0, 1, 127)), at(48, cc(0, 1, 0))});
    const float turned = source.get_output_at(47);
    EXPECT_NEAR(turned, 0.5f, 1e-5f);
    EXPECT_LT(source.get_output_at(48), turned);
    EXPECT_NEAR(source.get_output_at(48), turned - turned / 96.0f, 1e-5f);

    // Times that are no time step, and a very long one neither overflows nor moves far.
    source.set_smoothing_ms(-5.0f);
    run_block(midi, source, {at(0, cc(0, 1, 127))});
    EXPECT_FLOAT_EQ(source.get_output_at(0), 1.0f);
    source.set_smoothing_ms(1.0e12f);
    run_block(midi, source, {at(0, cc(0, 1, 0))});
    EXPECT_NEAR(source.get_output_at(k_bs - 1), 1.0f, 1e-3f);
}

// Soft takeover: armed at the parameter's position, the source stays out until the knob
// gets there, from whichever side.
TEST(MidiControllerSource, PickupWaitsUntilTheControllerReachesThePosition) {
    Midi midi;
    MidiControllerSource source(midi.m_stream, controller_address(7));
    source.prepare(k_sr, k_bs, 1);
    source.arm_pickup(0.5f);
    EXPECT_TRUE(source.waiting_for_pickup());

    run_block(midi, source, {at(0, cc(0, 7, 13)), at(8, cc(0, 7, 40))});
    EXPECT_EQ(source.get_output_active_at(k_bs - 1), 0);
    EXPECT_TRUE(source.waiting_for_pickup());

    run_block(midi, source, {at(4, cc(0, 7, 70))});
    EXPECT_EQ(source.get_output_active_at(3), 0);
    EXPECT_EQ(source.get_output_active_at(4), 1);
    EXPECT_FLOAT_EQ(source.get_output_at(4), 70.0f / 127.0f);
    EXPECT_FALSE(source.waiting_for_pickup());

    // Picked up, it follows wherever the knob goes.
    run_block(midi, source, {at(0, cc(0, 7, 5))});
    EXPECT_FLOAT_EQ(source.get_output_at(0), 5.0f / 127.0f);

    // From above.
    source.arm_pickup(0.5f);
    run_block(midi, source, {at(0, cc(0, 7, 100)), at(8, cc(0, 7, 80))});
    EXPECT_EQ(source.get_output_active_at(0), 0);
    EXPECT_EQ(source.get_change_points(), (std::vector<uint32_t>{0}));
    run_block(midi, source, {at(0, cc(0, 7, 60))});
    EXPECT_EQ(source.get_output_active_at(0), 1);
    EXPECT_FLOAT_EQ(source.get_output_at(0), 60.0f / 127.0f);
}

// A knob that already stands at the position takes over with its first move.
TEST(MidiControllerSource, PickupIsImmediateWhenTheControllerIsAlreadyThere) {
    Midi midi;
    MidiControllerSource source(midi.m_stream, controller_address(7));
    source.prepare(k_sr, k_bs, 1);
    source.arm_pickup(0.5f);
    run_block(midi, source, {at(0, cc(0, 7, 64))});
    EXPECT_EQ(source.get_output_active_at(0), 1);

    // Bipolar output has twice the range, and so twice the window.
    MidiControllerSource bipolar(midi.m_stream, controller_address(7), MidiPolarity::Bipolar);
    bipolar.prepare(k_sr, k_bs, 1);
    bipolar.arm_pickup(0.0f);
    run_block(midi, bipolar, {at(0, cc(0, 7, 65))});  // 0.024, inside twice 0.012
    EXPECT_EQ(bipolar.get_output_active_at(0), 1);
}

TEST(MidiControllerSource, DisarmedPickupLetsTheNextMoveTakeOver) {
    Midi midi;
    MidiControllerSource source(midi.m_stream, controller_address(7));
    source.prepare(k_sr, k_bs, 1);
    source.arm_pickup(0.9f);
    run_block(midi, source, {at(0, cc(0, 7, 10))});
    EXPECT_EQ(source.get_output_active_at(0), 0);

    source.disarm_pickup();
    EXPECT_FALSE(source.waiting_for_pickup());
    run_block(midi, source, {at(0, cc(0, 7, 11))});
    EXPECT_EQ(source.get_output_active_at(0), 1);
}

// A pickup that is still waiting carries over to a newly learned controller.
TEST(MidiControllerSource, WaitingPickupSurvivesANewAddress) {
    Midi midi;
    MidiControllerSource source(midi.m_stream, controller_address(7));
    source.prepare(k_sr, k_bs, 1);
    source.arm_pickup(0.5f);
    run_block(midi, source, {at(0, cc(0, 7, 120))});

    source.set_address(controller_address(21));
    run_block(midi, source, {at(0, cc(0, 21, 10))});
    EXPECT_EQ(source.get_output_active_at(0), 0);
    // It was above on the old knob; the new one is looked at afresh and comes from below.
    run_block(midi, source, {at(0, cc(0, 21, 64))});
    EXPECT_EQ(source.get_output_active_at(0), 1);
}

// Through the matrix, soft takeover means the parameter does not move until the knob
// reaches it.
TEST(MidiControllerSource, PickupKeepsTheParameterStillUntilTheKnobArrives) {
    FakeBackend backend;
    backend.add("cutoff", 0.5f);
    ModulationMatrix matrix(backend);
    Midi midi;
    MidiControllerSource source(midi.m_stream, controller_address(74));
    matrix.add_source("midi.cc.74", &source);
    auto cutoff = matrix.get_smart_handle<float>("cutoff");
    ModulationRouting routing("midi.cc.74", "cutoff", 1.0f);
    routing.m_combine_mode = CombineMode::Replace;
    matrix.add_routing(routing);
    matrix.prepare(k_sr, k_bs);
    source.arm_pickup(0.5f);

    midi.block({at(0, cc(0, 74, 0)), at(20, cc(0, 74, 30))});
    matrix.process(k_bs);
    for (uint32_t i = 0; i < k_bs; ++i) { EXPECT_FLOAT_EQ(cutoff.load(i), 0.5f); }

    midi.block({at(10, cc(0, 74, 63))});
    matrix.process(k_bs);
    EXPECT_FLOAT_EQ(cutoff.load(9), 0.5f);
    EXPECT_FLOAT_EQ(cutoff.load(10), 63.0f / 127.0f);
    matrix.remove_source("midi.cc.74");
}

// A host may send blocks shorter than prepared for. The stream says how long the block
// is, so a glide takes its time in samples, not in blocks.
TEST(MidiControllerSource, GlideKeepsItsTimeInShortBlocks) {
    constexpr size_t k_short = 16;
    Midi midi;
    MidiControllerSource source(midi.m_stream, controller_address(1));
    source.set_smoothing_ms(2.0f);  // 96 samples
    source.prepare(k_sr, k_bs, 1);

    midi.block({at(0, cc(0, 1, 0))}, k_short);
    source.pre_process_block();
    midi.block({at(0, cc(0, 1, 127))}, k_short);
    source.pre_process_block();
    EXPECT_NEAR(source.get_output_at(k_short - 1), 16.0f / 96.0f, 1e-5f);

    // Five more blocks of 16: sample 96 of the glide is the last of the sixth block.
    for (int block = 0; block < 4; ++block) {
        midi.block({}, k_short);
        source.pre_process_block();
    }
    EXPECT_NEAR(source.get_output_at(k_short - 1), 80.0f / 96.0f, 1e-5f);
    midi.block({}, k_short);
    source.pre_process_block();
    EXPECT_NEAR(source.get_output_at(k_short - 2), 95.0f / 96.0f, 1e-5f);
    EXPECT_FLOAT_EQ(source.get_output_at(k_short - 1), 1.0f);
    EXPECT_FLOAT_EQ(source.value(), 1.0f);
}

// The gate's one-sample gap on the last sample of a short block reopens in the next.
TEST(MidiNoteSource, RetriggerOnTheLastSampleOfAShortBlock) {
    constexpr size_t k_short = 16;
    Midi midi(1);
    MidiNoteSource gate(midi.m_stream, MidiNoteStream::Gate, k_voice_scope);
    gate.prepare(k_sr, k_bs, 1);

    midi.block({at(0, note_on(0, 60)), at(k_short - 1, note_on(0, 72))}, k_short);
    gate.pre_process_block();
    EXPECT_FLOAT_EQ(gate.voice_output(0)[k_short - 2], 1.0f);
    EXPECT_FLOAT_EQ(gate.voice_output(0)[k_short - 1], 0.0f);

    midi.block({}, k_short);
    gate.clear_per_block();
    gate.pre_process_block();
    EXPECT_FLOAT_EQ(gate.voice_output(0)[0], 1.0f);
    EXPECT_EQ(change_points(gate, 0), (std::vector<uint32_t>{0}));
}

// MIDI learn in one step: the new controller and its pickup arrive together, so the old
// knob cannot use the pickup up and the new one cannot move the target before it is armed.
TEST(MidiControllerSource, BindSetsControllerAndPickupTogether) {
    Midi midi;
    MidiControllerSource source(midi.m_stream, controller_address(7));
    source.prepare(k_sr, k_bs, 1);
    run_block(midi, source, {at(0, cc(0, 7, 64))});
    EXPECT_EQ(source.get_output_active_at(0), 1);

    source.bind(controller_address(21, 3), 0.5f);
    EXPECT_EQ(source.address(), controller_address(21, 3));
    EXPECT_TRUE(source.waiting_for_pickup());

    // The old knob at the pickup position no longer counts, the new one is not there yet.
    run_block(midi, source, {at(0, cc(0, 7, 64)), at(1, cc(3, 21, 10))});
    EXPECT_EQ(source.get_output_active_at(k_bs - 1), 0);
    run_block(midi, source, {at(0, cc(3, 21, 64))});
    EXPECT_EQ(source.get_output_active_at(0), 1);
    EXPECT_FALSE(source.waiting_for_pickup());

    // Arming the same position again is a new request.
    source.arm_pickup(0.5f);
    run_block(midi, source, {at(0, cc(3, 21, 120))});
    EXPECT_EQ(source.get_output_active_at(0), 0);
}
