#pragma once

#include <tanh/core/AbiNamespace.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace thl::inline THL_ABI_NAMESPACE::midi {

/// Message type of a Universal MIDI Packet: the top nibble of its first word.
enum class MessageType : std::uint8_t {
    Utility = 0x0,            ///< NOOP, jitter-reduction clock and timestamp (1 word).
    System = 0x1,             ///< System common and system real-time (1 word).
    Midi1ChannelVoice = 0x2,  ///< MIDI 1.0 channel voice, 7-bit values (1 word).
    SysEx7 = 0x3,             ///< Up to six bytes of a 7-bit System Exclusive (2 words).
    Midi2ChannelVoice = 0x4,  ///< MIDI 2.0 channel voice, 16/32-bit values (2 words).
    Data128 = 0x5,            ///< 8-bit System Exclusive and mixed data set (4 words).
    FlexData = 0xD,           ///< Flex data: tempo, time signature, text (4 words).
    Stream = 0xF,             ///< UMP stream: endpoint and function block discovery (4 words).
};

/// Channel voice opcode: the high nibble of the status byte of a MIDI 1.0 (type 2) or
/// MIDI 2.0 (type 4) channel voice message. The opcodes below NoteOff exist in MIDI 2.0
/// only, as does PerNoteManagement.
enum class ChannelVoiceOpcode : std::uint8_t {
    RegisteredPerNoteController = 0x0,
    AssignablePerNoteController = 0x1,
    RegisteredController = 0x2,  ///< RPN as one message.
    AssignableController = 0x3,  ///< NRPN as one message.
    RelativeRegisteredController = 0x4,
    RelativeAssignableController = 0x5,
    PerNotePitchBend = 0x6,
    NoteOff = 0x8,
    NoteOn = 0x9,
    PolyPressure = 0xA,
    ControlChange = 0xB,
    ProgramChange = 0xC,
    ChannelPressure = 0xD,
    PitchBend = 0xE,
    PerNoteManagement = 0xF,
};

/// Position of a SysEx7 packet (type 3) within its System Exclusive message.
enum class SysEx7Status : std::uint8_t {
    Complete = 0x0,  ///< The whole message fits this packet.
    Start = 0x1,
    Continue = 0x2,
    End = 0x3,
};

/// Number of 32-bit words of the packet that starts with `first_word` (1 to 4), from its
/// message type. Reserved message types have the sizes the UMP specification assigns them.
[[nodiscard]] constexpr std::uint8_t ump_word_count(std::uint32_t first_word) noexcept {
    constexpr std::array<std::uint8_t, 16> k_word_counts =
        {1, 1, 1, 2, 2, 4, 1, 1, 2, 2, 2, 3, 3, 4, 4, 4};
    return k_word_counts[first_word >> 28U];
}

/// One complete Universal MIDI Packet — the form every MIDI message has inside tanh-lib.
///
/// MIDI 1.0 input is carried as type 2 (and type 1 / type 3 for system and System
/// Exclusive) without any change of resolution, MIDI 2.0 input as type 4. Words are in
/// host byte order; unused words are zero, so two packets compare equal exactly when they
/// are the same message.
struct Ump {
    std::array<std::uint32_t, 4> m_words{};  ///< The packet, m_size words of it valid.
    std::uint8_t m_size = 0;                 ///< Number of valid words, 1 to 4 (0 = empty).

    [[nodiscard]] constexpr MessageType message_type() const noexcept {
        return static_cast<MessageType>(m_words[0] >> 28U);
    }
    /// Group, 0 to 15.
    [[nodiscard]] constexpr std::uint8_t group() const noexcept {
        return static_cast<std::uint8_t>((m_words[0] >> 24U) & 0xFU);
    }
    /// The status byte: opcode and channel for channel voice messages, the MIDI 1.0 status
    /// (0xF1 to 0xFF) for system messages.
    [[nodiscard]] constexpr std::uint8_t status() const noexcept {
        return static_cast<std::uint8_t>((m_words[0] >> 16U) & 0xFFU);
    }
    /// Opcode of a channel voice message (type 2 or 4).
    [[nodiscard]] constexpr ChannelVoiceOpcode opcode() const noexcept {
        return static_cast<ChannelVoiceOpcode>((m_words[0] >> 20U) & 0xFU);
    }
    /// Channel of a channel voice message, 0 to 15.
    [[nodiscard]] constexpr std::uint8_t channel() const noexcept {
        return static_cast<std::uint8_t>((m_words[0] >> 16U) & 0xFU);
    }
    /// Third byte of the first word: note number, controller index, first data byte.
    [[nodiscard]] constexpr std::uint8_t data1() const noexcept {
        return static_cast<std::uint8_t>((m_words[0] >> 8U) & 0xFFU);
    }
    /// Fourth byte of the first word: second MIDI 1.0 data byte, or the MIDI 2.0 attribute
    /// type / controller index / option flags.
    [[nodiscard]] constexpr std::uint8_t data2() const noexcept {
        return static_cast<std::uint8_t>(m_words[0] & 0xFFU);
    }

    friend constexpr bool operator==(const Ump&, const Ump&) noexcept = default;
};

/// A packet with the time it arrived and the input it came from: the element of
/// InputQueue.
struct TimedUmp {
    /// Arrival time in nanoseconds, in the time base of thl::core::uptime_ns().
    std::int64_t m_time_ns = 0;
    Ump m_ump;
    /// Slot of the input the packet came from, assigned by whoever pushes it.
    std::uint16_t m_source = 0;
};

static_assert(std::is_trivially_copyable_v<TimedUmp>);
static_assert(sizeof(TimedUmp) == 32);

/// A system message (type 1): system common or system real-time. `status` is the MIDI 1.0
/// status byte, 0xF1 to 0xFF.
[[nodiscard]] constexpr Ump make_system(std::uint8_t group,
                                        std::uint8_t status,
                                        std::uint8_t data1 = 0,
                                        std::uint8_t data2 = 0) noexcept {
    Ump ump;
    ump.m_words[0] = (0x1U << 28U) | ((group & 0xFU) << 24U) |
                     (static_cast<std::uint32_t>(status) << 16U) | ((data1 & 0x7FU) << 8U) |
                     (data2 & 0x7FU);
    ump.m_size = 1;
    return ump;
}

/// A MIDI 1.0 channel voice message (type 2). `status` is the MIDI 1.0 status byte, 0x80
/// to 0xEF; `data2` is 0 for program change and channel pressure.
[[nodiscard]] constexpr Ump make_midi1_channel_voice(std::uint8_t group,
                                                     std::uint8_t status,
                                                     std::uint8_t data1,
                                                     std::uint8_t data2 = 0) noexcept {
    Ump ump;
    ump.m_words[0] = (0x2U << 28U) | ((group & 0xFU) << 24U) |
                     (static_cast<std::uint32_t>(status) << 16U) | ((data1 & 0x7FU) << 8U) |
                     (data2 & 0x7FU);
    ump.m_size = 1;
    return ump;
}

/// A MIDI 2.0 channel voice message (type 4). `index` and `extra` are the third and fourth
/// byte of the first word (note number and attribute type, controller bank and index, ...),
/// `data` is the second word.
[[nodiscard]] constexpr Ump make_midi2_channel_voice(std::uint8_t group,
                                                     ChannelVoiceOpcode opcode,
                                                     std::uint8_t channel,
                                                     std::uint8_t index,
                                                     std::uint8_t extra,
                                                     std::uint32_t data) noexcept {
    Ump ump;
    ump.m_words[0] = (0x4U << 28U) | ((group & 0xFU) << 24U) |
                     (static_cast<std::uint32_t>(opcode) << 20U) | ((channel & 0xFU) << 16U) |
                     (static_cast<std::uint32_t>(index) << 8U) | extra;
    ump.m_words[1] = data;
    ump.m_size = 2;
    return ump;
}

/// A SysEx7 packet (type 3) carrying `count` (0 to 6) payload bytes, without the 0xF0 and
/// 0xF7 framing bytes.
[[nodiscard]] constexpr Ump make_sysex7(std::uint8_t group,
                                        SysEx7Status status,
                                        const std::array<std::uint8_t, 6>& bytes,
                                        std::uint8_t count) noexcept {
    Ump ump;
    ump.m_words[0] = (0x3U << 28U) | ((group & 0xFU) << 24U) |
                     (static_cast<std::uint32_t>(status) << 20U) | ((count & 0xFU) << 16U) |
                     ((bytes[0] & 0x7FU) << 8U) | (bytes[1] & 0x7FU);
    ump.m_words[1] = ((bytes[2] & 0x7FU) << 24U) | ((bytes[3] & 0x7FU) << 16U) |
                     ((bytes[4] & 0x7FU) << 8U) | (bytes[5] & 0x7FU);
    ump.m_size = 2;
    return ump;
}

/// Number of payload bytes of a SysEx7 packet, 0 to 6.
[[nodiscard]] constexpr std::uint8_t sysex7_byte_count(const Ump& ump) noexcept {
    return static_cast<std::uint8_t>((ump.m_words[0] >> 16U) & 0xFU);
}

/// Payload byte `index` (0 to 5) of a SysEx7 packet.
[[nodiscard]] constexpr std::uint8_t sysex7_byte(const Ump& ump, std::uint8_t index) noexcept {
    const std::uint64_t payload =
        (static_cast<std::uint64_t>(ump.m_words[0] & 0xFFFFU) << 32U) | ump.m_words[1];
    return static_cast<std::uint8_t>((payload >> (40U - 8U * index)) & 0xFFU);
}

/// Splits a stream of UMP words (host byte order) into packets and calls `sink(const Ump&)`
/// for each. A packet cut off by the end of the stream is dropped. Returns the number of
/// packets delivered.
template <typename Sink>
std::size_t for_each_ump(const std::uint32_t* words, std::size_t count, Sink&& sink) {
    std::size_t delivered = 0;
    std::size_t pos = 0;
    while (pos < count) {
        const std::uint8_t size = ump_word_count(words[pos]);
        if (count - pos < size) { break; }
        Ump ump;
        for (std::uint8_t i = 0; i < size; ++i) { ump.m_words[i] = words[pos + i]; }
        ump.m_size = size;
        sink(static_cast<const Ump&>(ump));
        pos += size;
        ++delivered;
    }
    return delivered;
}

}  // namespace thl::inline THL_ABI_NAMESPACE::midi
