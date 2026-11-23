#pragma once

#include <tanh/core/AbiNamespace.h>
#include <tanh/core/Exports.h>
#include <tanh/midi/Event.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace thl::inline THL_ABI_NAMESPACE::midi {

/// Gives every note a voice and tells each voice what the controller means for it: the
/// step between "MIDI arrived" and "voice 3 plays this pitch with this pressure".
///
/// Events go in as EventDecoder made them and come out with Event::m_voice set:
///
/// - NoteOn takes a free voice, the one released longest ago, or steals the voice of the
///   oldest note when all are busy. A stolen note gets no NoteOff: the NoteOn on a voice
///   that still sounds is the steal. NoteOff carries the voice it frees, or k_no_voice when
///   its note was stolen before.
/// - After every NoteOn, and whenever one of them changes, each voice gets three
///   NoteExpression events: Tuning (semitones away from its key), Pressure and Timbre.
///   They are resolved from whatever reaches the note:
///   - Tuning: the note's channel bend times that channel's range, the note's own bend
///     (MIDI 2.0) times per_note_bend_range(), its own pitch (MIDI 2.0), and in an MPE
///     zone the manager channel's bend times the manager's range.
///   - Pressure: the largest of the note's polyphonic pressure, its channel's pressure and,
///     in an MPE zone, the manager channel's.
///   - Timbre: the note's own (MIDI 2.0), else in an MPE zone the member channel's CC 74
///     moved by the manager's CC 74 around its centre, else 0.5.
/// - The pitch bend range of a channel is 2 semitones until RPN 0 sets it.
///
/// MPE (MIDI Polyphonic Expression, RP-053): a zone is a manager channel (1 for the lower
/// zone, 16 for the upper) and a number of member channels next to it, one note per member
/// channel. An MPE Configuration Message (RPN 6 on a manager channel) sets a zone, as
/// set_zones() does by hand; either resets the members' bend range to 48 semitones and
/// every other channel's to 2. Pitch bend, channel pressure and CC 74 on a member channel
/// are that note's expression and are not passed on as channel events; on every other
/// channel they are passed on as well.
///
/// Pedals: while the sustain pedal (CC 64) of a note's channel is down, a NoteOff does not
/// free its voice: it passes with k_no_voice, the voice sounds on, and the NoteOff with the
/// voice follows when the pedal comes up. The sostenuto pedal (CC 66) does the same for
/// the notes whose keys were down when it was pressed, and for those only. A pedal on an
/// MPE manager channel holds the whole zone. Both control changes are passed on as well;
/// set_follow_pedals(false) leaves it at that. When all voices are busy, voices held only
/// by a pedal are stolen before voices whose keys are down.
///
/// All Notes Off and All Sound Off (CC 123, 120) release the voices of their channel, and
/// of the whole zone when sent on a manager channel, pedal or not. Reset All Controllers
/// (CC 121) puts the channel's bend, pressure, CC 74 and pedals back to rest.
///
/// One allocator serves one stream of notes: channel state is kept per channel, whatever
/// the group or input, so two MPE controllers at once need an allocator each.
///
/// Threading: process(), begin_block(), release_all() and voice() on the audio thread
/// (no allocation, no locks); set_voice_count() and reset() with the audio stopped; the
/// setters and zone getters on any thread.
class TANH_API VoiceAllocator {
public:
    /// Upper limit of set_voice_count().
    static constexpr std::size_t k_max_voices = 64;
    /// Pitch bend range of an ordinary channel and of an MPE manager channel, in semitones.
    static constexpr float k_channel_bend_range = 2.0f;
    /// Pitch bend range of an MPE member channel, in semitones.
    static constexpr float k_member_bend_range = 48.0f;

    /// What a voice plays, for whoever renders it.
    struct Voice {
        bool m_active = false;       ///< Between its NoteOn and NoteOff.
        bool m_key_down = false;     ///< Its key is down; false while only a pedal holds it.
        std::uint8_t m_group = 0;    ///< Of the note it plays or played last.
        std::uint8_t m_channel = 0;  ///< Of the note it plays or played last.
        std::uint8_t m_key = 0;      ///< Of the note it plays or played last.
        std::uint16_t m_source = 0;  ///< Input of the note it plays or played last.
        float m_velocity = 0.0f;     ///< Of its NoteOn.
        float m_tuning = 0.0f;       ///< Last Tuning sent, in semitones.
        float m_pressure = 0.0f;     ///< Last Pressure sent.
        float m_timbre = 0.5f;       ///< Last Timbre sent.
    };

    VoiceAllocator();

    /// Number of voices notes are spread over, 1 to k_max_voices. Forgets every note.
    void set_voice_count(std::uint32_t count) noexcept;
    /// Number of voices notes are spread over.
    [[nodiscard]] std::uint32_t voice_count() const noexcept { return m_voice_count; }

    /// Sets the MPE zones by hand: member channels of the lower and of the upper zone, 0
    /// (zone off) to 15. Takes effect at the next begin_block(). Where the two overlap
    /// the lower zone wins.
    void set_zones(std::uint8_t lower_members, std::uint8_t upper_members) noexcept;
    /// Whether an MPE Configuration Message from the controller sets the zones (default:
    /// yes).
    void set_follow_configuration_messages(bool follow) noexcept {
        m_follow_configuration.store(follow, std::memory_order_relaxed);
    }
    /// Member channels of the lower zone as they are now, 0 when it is off.
    [[nodiscard]] std::uint8_t lower_zone_members() const noexcept {
        return m_lower_members.load(std::memory_order_relaxed);
    }
    /// Member channels of the upper zone as they are now, 0 when it is off.
    [[nodiscard]] std::uint8_t upper_zone_members() const noexcept {
        return m_upper_members.load(std::memory_order_relaxed);
    }

    /// Whether the sustain and sostenuto pedals (CC 64, 66) hold notes (default: yes).
    /// Switched off while a pedal holds notes, those end at the next pedal event of their
    /// channel or at release_all().
    void set_follow_pedals(bool follow) noexcept {
        m_follow_pedals.store(follow, std::memory_order_relaxed);
    }

    /// Range of a MIDI 2.0 per-note pitch bend in semitones (default 48). MIDI 2.0 sets it
    /// with a registered controller of its own, which is not read yet.
    void set_per_note_bend_range(float semitones) noexcept {
        m_per_note_bend_range.store(semitones, std::memory_order_relaxed);
    }
    /// Range of a MIDI 2.0 per-note pitch bend in semitones.
    [[nodiscard]] float per_note_bend_range() const noexcept {
        return m_per_note_bend_range.load(std::memory_order_relaxed);
    }

    /// Applies a set_zones() request, if one is waiting, and tells the voices at sample 0.
    void begin_block(EventWriter& out) noexcept TANH_NONBLOCKING_FUNCTION;

    /// One event in, the events it means for the voices out (see the class description).
    void process(const Event& event, EventWriter& out) noexcept TANH_NONBLOCKING_FUNCTION;

    /// A NoteOff at `sample_offset` for every voice that has played, sounding or not, and
    /// both pedals up on every channel: for input that lost messages
    /// (InputQueue::dropped() rose, EventStream::dropped() rose) and for a panic button.
    void release_all(std::uint32_t sample_offset,
                     EventWriter& out) noexcept TANH_NONBLOCKING_FUNCTION;

    /// Forgets every note and all channel state without emitting anything. The zones stay.
    void reset() noexcept;

    /// Voice `index`, below voice_count().
    [[nodiscard]] const Voice& voice(std::size_t index) const noexcept {
        return m_voices[index].m_public;
    }

private:
    struct VoiceState {
        Voice m_public;
        std::uint64_t m_stamp = 0;  // order of its note on while active, of its release after
        float m_poly_pressure = 0.0f;
        float m_note_bend = 0.0f;     // MIDI 2.0 per-note pitch bend, -1 to 1
        float m_pitch_offset = 0.0f;  // MIDI 2.0 pitch minus the key, in semitones
        float m_note_timbre = -1.0f;  // MIDI 2.0 per-note timbre; negative: none received
        bool m_sostenuto = false;     // its key was down when the sostenuto pedal went down
    };

    struct ChannelState {
        float m_bend = 0.0f;
        float m_pressure = 0.0f;
        float m_timbre = 0.5f;
        float m_bend_range = k_channel_bend_range;
        bool m_sustain = false;    // CC 64 is down
        bool m_sostenuto = false;  // CC 66 is down
    };

    [[nodiscard]] bool is_member(std::uint8_t channel) const noexcept;
    /// Manager channel of the zone `channel` is a member of, or -1.
    [[nodiscard]] int manager_of(std::uint8_t channel) const noexcept;
    /// True when `channel` manages a zone that is on.
    [[nodiscard]] bool is_manager(std::uint8_t channel) const noexcept;
    /// True when a change on `channel` reaches the note of `voice`.
    [[nodiscard]] bool reaches(std::uint8_t channel, const VoiceState& voice) const noexcept;

    [[nodiscard]] int find(const Event& note) const noexcept;
    [[nodiscard]] std::size_t allocate() const noexcept;

    void note_on(const Event& event, EventWriter& out) noexcept;
    void note_off(const Event& event, EventWriter& out) noexcept;
    void note_expression(const Event& event, EventWriter& out) noexcept;
    void controller(const Event& event, EventWriter& out) noexcept;
    void registered_controller(const Event& event, EventWriter& out) noexcept;

    /// True when a pedal keeps the note of `voice` sounding without its key.
    [[nodiscard]] bool held_by_pedal(const VoiceState& voice) const noexcept;
    /// Ends the voices `channel` reaches that neither a key nor a pedal holds any more.
    void release_unheld(std::uint8_t channel,
                        std::uint32_t sample_offset,
                        EventWriter& out) noexcept;
    void end_voice(std::size_t index, std::uint32_t sample_offset, EventWriter& out) noexcept;

    void apply_zones(std::uint8_t lower, std::uint8_t upper, bool lower_wins) noexcept;
    void release_channel(std::uint8_t channel,
                         std::uint32_t sample_offset,
                         EventWriter& out) noexcept;
    /// Sends the expressions of every voice `channel` reaches that changed.
    void update_channel(std::uint8_t channel,
                        std::uint32_t sample_offset,
                        EventWriter& out) noexcept;
    /// Sends the expressions of one voice: all three with `force`, else those that changed.
    void update_voice(std::size_t index,
                      std::uint32_t sample_offset,
                      bool force,
                      EventWriter& out) noexcept;

    std::array<VoiceState, k_max_voices> m_voices{};
    std::array<ChannelState, 16> m_channels{};
    std::uint64_t m_counter = 0;
    std::uint32_t m_voice_count = 16;

    std::atomic<std::uint8_t> m_lower_members{0};
    std::atomic<std::uint8_t> m_upper_members{0};
    std::atomic<std::uint32_t> m_zone_request{0};  // 0: none; else 1 << 16 | upper << 8 | lower
    std::atomic<bool> m_follow_configuration{true};
    std::atomic<bool> m_follow_pedals{true};
    std::atomic<float> m_per_note_bend_range{k_member_bend_range};
};

}  // namespace thl::inline THL_ABI_NAMESPACE::midi
