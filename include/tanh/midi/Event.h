#pragma once

#include <tanh/core/AbiNamespace.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace thl::inline THL_ABI_NAMESPACE::midi {

/// What an Event says. The value ranges are those of Event::m_value.
enum class EventType : std::uint8_t {
    NoteOn,   ///< m_key starts; value: velocity, 0 to 1.
    NoteOff,  ///< m_key ends; value: release velocity, 0 to 1.
    /// A value of one note: m_key, and m_index holds an Expression.
    NoteExpression,
    Controller,  ///< Control change m_index (0 to 127); value 0 to 1.
    /// RPN m_index (bank * 128 + index); value 0 to 1, where 1 is a data entry MSB of 127
    /// (MIDI 1.0) or all 32 bits set (MIDI 2.0).
    RegisteredController,
    AssignableController,  ///< NRPN m_index (bank * 128 + index); value as for an RPN.
    PitchBend,             ///< The channel's pitch bend; value -1 to 1, 0 at rest.
    ChannelPressure,       ///< The channel's pressure (aftertouch); value 0 to 1.
    ProgramChange,         ///< Program m_index (0 to 127); value unused.
};

/// Which value of a note a NoteExpression event carries (Event::m_index).
///
/// EventDecoder emits the first four as they are on the wire; VoiceAllocator resolves them,
/// together with the channel-wide messages, into Tuning, Pressure and Timbre per voice.
enum class Expression : std::uint8_t {
    Pressure,   ///< 0 to 1 (polyphonic key pressure).
    Timbre,     ///< 0 to 1, 0.5 at rest (MIDI 2.0 per-note controller 74; CC 74 in MPE).
    PitchBend,  ///< -1 to 1 (MIDI 2.0 per-note pitch bend), still without a range.
    Pitch,      ///< The note's own pitch in semitones, 0 to 128 (MIDI 2.0 Pitch 7.9 / 7.25).
    /// Semitones the note sounds away from its key: every bend that reaches it, each
    /// times its range, plus its own pitch. Emitted by VoiceAllocator only.
    Tuning,
};

/// Event::m_voice of an event that belongs to no voice.
inline constexpr std::int16_t k_no_voice = -1;

/// One MIDI message as a synthesizer wants it: what happened, to which note or
/// controller, as a float, at a sample of the block.
///
/// EventDecoder makes these from Universal MIDI Packets, so MIDI 1.0 and MIDI 2.0 input
/// look the same from here on; VoiceAllocator then gives every note a voice.
struct Event {
    std::uint32_t m_sample_offset = 0;  ///< Position in the block.
    float m_value = 0.0f;               ///< See EventType and Expression.
    /// The message's value at full resolution, left-aligned in 32 bits: a MIDI 1.0 value of
    /// 7 bits is in the top 7 bits, a 14-bit value (pitch bend, RPN data) in the top 14.
    /// An RPN's data entry MSB is therefore `m_data >> 25` whatever the protocol.
    std::uint32_t m_data = 0;
    std::uint16_t m_index = 0;          ///< Controller, RPN / NRPN, program, or Expression.
    std::uint16_t m_source = 0;         ///< The input it came from (BlockEvent::m_source).
    std::int16_t m_voice = k_no_voice;  ///< Voice of the note, once VoiceAllocator set it.
    EventType m_type = EventType::Controller;
    std::uint8_t m_group = 0;    ///< UMP group, 0 to 15.
    std::uint8_t m_channel = 0;  ///< 0 to 15.
    std::uint8_t m_key = 0;      ///< Note number, 0 to 127, for the note events.

    /// The Expression of a NoteExpression event.
    [[nodiscard]] constexpr Expression expression() const noexcept {
        return static_cast<Expression>(m_index);
    }
};

static_assert(std::is_trivially_copyable_v<Event>);
static_assert(sizeof(Event) == 24);

/// Appends events to storage somebody else owns, and counts what did not fit. The stages
/// of the event path (EventDecoder, VoiceAllocator) write through one of these, so none of
/// them allocates.
class EventWriter {
public:
    /// Writes into `storage`, starting empty.
    explicit EventWriter(std::span<Event> storage) noexcept : m_storage(storage) {}

    /// Appends `event`. Returns false, and counts a drop, when the storage is full.
    bool push(const Event& event) noexcept {
        if (m_size == m_storage.size()) {
            ++m_dropped;
            return false;
        }
        m_storage[m_size++] = event;
        return true;
    }

    /// The events written so far.
    [[nodiscard]] std::span<const Event> events() const noexcept { return m_storage.first(m_size); }
    /// Number of events written so far.
    [[nodiscard]] std::size_t size() const noexcept { return m_size; }
    /// Events that did not fit.
    [[nodiscard]] std::size_t dropped() const noexcept { return m_dropped; }

private:
    std::span<Event> m_storage;
    std::size_t m_size = 0;
    std::size_t m_dropped = 0;
};

}  // namespace thl::inline THL_ABI_NAMESPACE::midi
