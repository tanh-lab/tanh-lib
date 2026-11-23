#pragma once

#include <tanh/core/AbiNamespace.h>
#include <tanh/core/Exports.h>
#include <tanh/midi/Event.h>
#include <tanh/midi/EventStream.h>
#include <tanh/modulation/ModulationSource.h>
#include <tanh/parameters/ModulationScope.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

// MIDI as modulation sources. Part of Modulation when the Midi component is built too
// (TANH_BUILD_MIDI); Modulation then links tanh::Midi.
//
// Both sources read the block's events from a midi::EventStream, which the application
// fills before ModulationMatrix::process():
//
//     stream.process(block_events, num_samples);  // packets -> events with voices
//     matrix.process(num_samples);                // the sources place them at their samples
//
// A source keeps a pointer to the stream: the stream outlives it.

namespace thl::inline THL_ABI_NAMESPACE::modulation {

/// The controller a MidiControllerSource follows.
struct MidiControllerAddress {
    /// Channel that matches every channel.
    static constexpr std::int8_t k_any_channel = -1;

    /// Controller, AssignableController (NRPN), PitchBend or ChannelPressure.
    midi::EventType m_type = midi::EventType::Controller;
    /// Controller (0 to 127) or NRPN number; unused for the other two.
    std::uint16_t m_index = 0;
    /// 0 to 15, or k_any_channel.
    std::int8_t m_channel = k_any_channel;

    friend constexpr bool operator==(const MidiControllerAddress&,
                                     const MidiControllerAddress&) noexcept = default;
};

/// Range of a MidiControllerSource's output.
enum class MidiPolarity : std::uint8_t {
    Unipolar,  ///< 0 to 1; pitch bend rests at 0.5. What a Replace routing's range maps.
    Bipolar    ///< -1 to 1; pitch bend rests at 0, a controller's centre is 0.
};

/// One MIDI controller as a global modulation source: a control change, an NRPN, the
/// pitch bend or the channel pressure, sample-accurate.
///
/// The output moves to each new value at the event's sample and holds it. Until the
/// first event the source is inactive, so a Replace routing leaves its target at the base
/// value until the controller is actually moved; from then on it stays active.
///
/// Smoothing (set_smoothing_ms(), off by default): instead of stepping, the output glides
/// to each new value in a straight line over that time, which takes the staircase out of
/// a 7-bit controller. The first value after the source became active is taken at once.
///
/// Soft takeover (arm_pickup()): a knob rarely stands where the parameter is. Armed with
/// the position the output should be picked up at, the source stays inactive until the
/// controller reaches that position or passes it, and only then starts to drive its
/// targets, so the parameter does not jump to wherever the knob happens to be.
///
/// MIDI learn: read midi::Monitor::last_controller(), put what it reports into
/// set_address() (or a new source), and route the source to the parameter.
///
/// A block is rendered as long as the stream says it is (EventStream::num_samples()), so a
/// glide keeps its time when the host sends blocks shorter than prepared.
///
/// Threading: prepare() with the audio stopped; pre_process_block() on the audio thread;
/// everything else on any thread.
class TANH_API MidiControllerSource : public ModulationSource {
public:
    /// How close to the pickup position counts as reached: one and a half steps of a
    /// 7-bit controller.
    static constexpr float k_pickup_window = 1.5f / 127.0f;
    /// While the output glides, a change point at least every this many samples.
    static constexpr std::uint32_t k_glide_change_point_interval = 32;

    MidiControllerSource(const midi::EventStream& stream,
                         MidiControllerAddress address,
                         MidiPolarity polarity = MidiPolarity::Unipolar);

    void prepare(double sample_rate, size_t samples_per_block, uint32_t voice_count) override;
    void pre_process_block() override;

    /// Follows another controller from the next block on; the source is inactive again
    /// until that one moves (or, with a pickup armed, until it reaches the pickup).
    void set_address(MidiControllerAddress address) noexcept;
    /// The controller the source follows.
    [[nodiscard]] MidiControllerAddress address() const noexcept;

    /// MIDI learn in one step: set_address() and arm_pickup() taking effect in the same
    /// block, so neither the old controller can use up the pickup nor the new one move
    /// the target before the pickup is armed.
    void bind(MidiControllerAddress address, float pickup_position) noexcept;

    /// Time the output takes to reach a new value, in milliseconds; 0 (the default) steps.
    void set_smoothing_ms(float milliseconds) noexcept {
        m_smoothing_ms.store(milliseconds, std::memory_order_relaxed);
    }
    [[nodiscard]] float smoothing_ms() const noexcept {
        return m_smoothing_ms.load(std::memory_order_relaxed);
    }

    /// Soft takeover: makes the source inactive until its controller reaches `position`
    /// (in the source's output range, usually where the target parameter stands now) or
    /// moves across it. Takes effect at the next block. The position is kept to 16 bits.
    void arm_pickup(float position) noexcept;
    /// Ends a pickup that is still waiting: the controller's next move takes over.
    void disarm_pickup() noexcept;
    /// True while an armed pickup has not been reached, for a "move the knob" hint.
    [[nodiscard]] bool waiting_for_pickup() const noexcept {
        return m_waiting.load(std::memory_order_relaxed);
    }

    /// The output at the end of the last block, for a meter.
    [[nodiscard]] float value() const noexcept { return m_value.load(std::memory_order_relaxed); }

private:
    enum class Pickup : std::uint8_t {
        None,       // no pickup: the next move takes over
        FirstLook,  // armed, the controller has not been seen yet
        Below,      // the controller is below the position
        Above       // the controller is above the position
    };

    [[nodiscard]] float output_of(const midi::Event& event) const noexcept;
    /// True when a controller at `value` may take over (and the pickup is done).
    [[nodiscard]] bool picks_up(float value) noexcept;
    /// Writes the output of samples [from, to), gliding where a glide is under way.
    void render(std::uint32_t from, std::uint32_t to);
    void mark(std::uint32_t offset);

    const midi::EventStream* m_stream;
    const MidiPolarity m_polarity;
    double m_sample_rate = 0.0;
    // What the other threads ask for, in one word so that it changes as one:
    // address << 32 | serial << 17 | pickup armed << 16 | pickup position (16 bits).
    std::atomic<std::uint64_t> m_binding;
    std::uint64_t m_bound = 0;  // the binding the state below belongs to
    float m_current = 0.0f;
    float m_target = 0.0f;
    float m_step = 0.0f;             // per sample while gliding
    std::uint32_t m_glide_left = 0;  // samples until the glide reaches m_target
    std::uint32_t m_since_mark = 0;  // samples since the glide's last change point
    bool m_active = false;
    Pickup m_pickup = Pickup::None;
    float m_pickup_position = 0.0f;
    std::atomic<float> m_smoothing_ms{0.0f};
    std::atomic<bool> m_waiting{false};
    std::atomic<float> m_value{0.0f};
};

/// What of a note a MidiNoteSource puts out.
enum class MidiNoteStream : std::uint8_t {
    Gate,      ///< 1 while the voice's note is held, 0 otherwise.
    Pitch,     ///< The note's key plus its tuning, mapped by set_pitch_range() to 0 to 1.
    Velocity,  ///< Of the note on, 0 to 1.
    Pressure,  ///< 0 to 1.
    Timbre     ///< 0 to 1, 0.5 at rest.
};

/// MIDI notes as a per-voice modulation source: voice v of the source's scope is voice v
/// of the EventStream's VoiceAllocator. One source per stream of a note; register the
/// ones the instrument uses.
///
/// Every stream is active on a voice from its note on to its note off and holds its last
/// value after that, so Replace hands the target back and ReplaceHold keeps the last note's
/// value. Events land on their sample.
///
/// A note on for a voice that is still sounding (the allocator stole it, or the same key
/// was struck again) closes the Gate for one sample before it opens again, so an envelope
/// behind it restarts. The other streams go straight to the new note's values.
///
/// MPE and MIDI 2.0 per-note expression need nothing extra here: the allocator has
/// resolved them into the Tuning, Pressure and Timbre of each voice.
///
/// Give the scope as many voices as the stream has (ModulationMatrix::set_voice_count(),
/// EventStreamConfig::m_voice_count); voices beyond the scope's count are ignored.
///
/// Threading: prepare() with the audio stopped; pre_process_block() on the audio thread;
/// set_pitch_range() on any thread.
class TANH_API MidiNoteSource : public ModulationSource {
public:
    MidiNoteSource(const midi::EventStream& stream, MidiNoteStream what, ModulationScope scope);

    void prepare(double sample_rate, size_t samples_per_block, uint32_t voice_count) override;
    void pre_process_block() override;

    /// The Pitch stream's range in semitones (MIDI note numbers): `low` gives 0, `high`
    /// gives 1, pitches outside are clamped. Default: 0 to 127.
    void set_pitch_range(float low, float high) noexcept;

    /// The stream of the notes this source puts out.
    [[nodiscard]] MidiNoteStream stream() const noexcept { return m_what; }

private:
    struct VoiceState {
        float m_value = 0.0f;
        float m_key = 0.0f;     // Pitch: the note's key
        float m_tuning = 0.0f;  // Pitch: its tuning in semitones
        bool m_active = false;
        bool m_reopen = false;  // Gate: closed on the block's last sample, opens on the next
    };

    [[nodiscard]] float pitch_value(const VoiceState& voice) const noexcept;

    const midi::EventStream* m_stream;
    const MidiNoteStream m_what;
    std::vector<VoiceState> m_voice_states;
    std::atomic<float> m_pitch_low{0.0f};
    std::atomic<float> m_pitch_high{127.0f};
};

}  // namespace thl::inline THL_ABI_NAMESPACE::modulation
