#pragma once

#include <tanh/core/AbiNamespace.h>
#include <tanh/core/Exports.h>
#include <tanh/midi/Event.h>
#include <tanh/midi/Ump.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <array>
#include <cstdint>

namespace thl::inline THL_ABI_NAMESPACE::midi {

/// Turns Universal MIDI Packets into Events: one float form for MIDI 1.0 (type 2) and
/// MIDI 2.0 (type 4) channel voice messages.
///
/// - Values keep the resolution they arrive with: 7 and 14 bits from MIDI 1.0, 16 and 32
///   bits from MIDI 2.0, scaled so that the lowest value is 0 (or -1) and the highest 1.
/// - A MIDI 1.0 note on with velocity 0 is a NoteOff.
/// - MIDI 1.0 selects an RPN or NRPN with CC 101 / 100 or 99 / 98 and sets it with CC 6
///   (MSB) and 38 (LSB). Those become one RegisteredController / AssignableController
///   event per data entry, as MIDI 2.0 sends them, and are not reported as Controller
///   events as well: CC 6 gives the MSB with an LSB of 0, CC 38 the MSB before it with its
///   LSB. A CC 38 with no CC 6 since the parameter was selected is ignored. With no
///   parameter selected, CC 6 and 38 are ordinary controllers.
/// - MIDI 2.0 per-note messages become NoteExpression events: polyphonic pressure,
///   per-note pitch bend, registered per-note controller 74 (Timbre) and 3 (Pitch 7.25),
///   and the Pitch 7.9 attribute of a note on. The other per-note controllers, relative
///   controllers and per-note management are ignored.
/// - Everything that is not a channel voice message (system, System Exclusive, utility)
///   is ignored.
///
/// One decoder per stream of packets: it holds the RPN / NRPN selection of every group
/// and channel. Not thread-safe; decode() does not allocate or block.
class TANH_API EventDecoder {
public:
    /// Decodes `ump` into zero, one or two events at `sample_offset` and appends them to
    /// `out`.
    void decode(const Ump& ump,
                std::uint32_t sample_offset,
                std::uint16_t source,
                EventWriter& out) noexcept TANH_NONBLOCKING_FUNCTION;

    /// Forgets every RPN / NRPN selection.
    void reset() noexcept TANH_NONBLOCKING_FUNCTION;

private:
    /// The parameter CC 6 / 38 of one channel address (MIDI 1.0 only).
    struct ParameterSelection {
        std::uint8_t m_msb = 0x7F;
        std::uint8_t m_lsb = 0x7F;
        std::uint8_t m_data_msb = 0;
        bool m_has_data_msb = false;  // CC 6 arrived since the parameter was selected
        bool m_registered = true;

        /// False for the null parameter (127, 127), which MIDI 1.0 uses to deselect.
        [[nodiscard]] bool selected() const noexcept { return m_msb != 0x7F || m_lsb != 0x7F; }
    };

    void decode_midi1(const Ump& ump, Event event, EventWriter& out) noexcept;
    void decode_midi2(const Ump& ump, Event event, EventWriter& out) noexcept;
    /// True when the control change was part of an RPN / NRPN and is handled.
    bool decode_parameter(std::uint8_t controller,
                          std::uint8_t value,
                          Event event,
                          EventWriter& out) noexcept;

    std::array<ParameterSelection, 256> m_parameters{};  // [group * 16 + channel]
};

}  // namespace thl::inline THL_ABI_NAMESPACE::midi
