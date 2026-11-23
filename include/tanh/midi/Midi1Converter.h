#pragma once

#include <tanh/core/AbiNamespace.h>
#include <tanh/core/Exports.h>
#include <tanh/midi/Ump.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace thl::inline THL_ABI_NAMESPACE::midi {

/// A MIDI 1.0 short message: status byte and up to two data bytes.
struct Midi1Message {
    std::array<std::uint8_t, 3> m_bytes{};
    std::uint8_t m_size = 0;  ///< Number of valid bytes, 1 to 3.

    friend constexpr bool operator==(const Midi1Message&, const Midi1Message&) noexcept = default;
};

/// The most MIDI 1.0 messages one packet converts to (a registered or assignable
/// controller: two selector CCs and two data-entry CCs).
inline constexpr std::size_t k_max_midi1_messages_per_ump = 4;

/// Output of convert_to_midi1().
using Midi1Messages = std::array<Midi1Message, k_max_midi1_messages_per_ump>;

/// Converts one packet to MIDI 1.0 short messages, for code that works on plain MIDI 1.0
/// whatever the source speaks. Returns the number of messages written to the front of
/// `out` (0 to 4).
///
/// - Type 1 (system) and type 2 (MIDI 1.0 channel voice) convert one to one.
/// - Type 4 (MIDI 2.0 channel voice) is scaled down by the MIDI 2.0 translation rules:
///   16-bit velocity and 32-bit values keep their top 7 (pitch bend: 14) bits, a note-on
///   whose velocity would become 0 gets velocity 1, a program change with a valid bank
///   becomes bank select MSB/LSB plus the program change, and a registered / assignable
///   controller becomes the RPN / NRPN sequence (CC 101/100 or 99/98, then CC 6 and 38).
/// - What MIDI 1.0 cannot express yields nothing: per-note controllers, per-note pitch
///   bend, per-note management and relative controllers, as well as System Exclusive
///   (type 3, which is not a short message) and every other message type.
[[nodiscard]] TANH_API std::size_t convert_to_midi1(const Ump& ump, Midi1Messages& out) noexcept
    TANH_NONBLOCKING_FUNCTION;

}  // namespace thl::inline THL_ABI_NAMESPACE::midi
