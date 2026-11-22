#include <tanh/core/AbiNamespace.h>
#include <tanh/midi/Midi1Converter.h>
#include <tanh/midi/Ump.h>

#include <cstddef>
#include <cstdint>

namespace thl::inline THL_ABI_NAMESPACE::midi {

namespace {

constexpr std::uint8_t k_cc_bank_msb = 0;
constexpr std::uint8_t k_cc_data_entry_msb = 6;
constexpr std::uint8_t k_cc_bank_lsb = 32;
constexpr std::uint8_t k_cc_data_entry_lsb = 38;
constexpr std::uint8_t k_cc_nrpn_lsb = 98;
constexpr std::uint8_t k_cc_nrpn_msb = 99;
constexpr std::uint8_t k_cc_rpn_lsb = 100;
constexpr std::uint8_t k_cc_rpn_msb = 101;

constexpr std::uint8_t k_status_control_change = 0xB0;
constexpr std::uint8_t k_status_program_change = 0xC0;

constexpr Midi1Message message(std::uint8_t status, std::uint8_t data1) noexcept {
    return Midi1Message{.m_bytes = {status, data1, 0}, .m_size = 2};
}

constexpr Midi1Message message(std::uint8_t status,
                               std::uint8_t data1,
                               std::uint8_t data2) noexcept {
    return Midi1Message{.m_bytes = {status, data1, data2}, .m_size = 3};
}

constexpr std::uint8_t low7(std::uint32_t value) noexcept {
    return static_cast<std::uint8_t>(value & 0x7FU);
}

// Number of bytes of the MIDI 1.0 message with this status byte; 0 when it is not a
// short message.
constexpr std::uint8_t midi1_message_size(std::uint8_t status) noexcept {
    if (status < 0x80) { return 0; }
    if (status < 0xF0) { return (status & 0xE0U) == 0xC0U ? 2 : 3; }
    switch (status) {
        case 0xF1:
        case 0xF3: return 2;
        case 0xF2: return 3;
        case 0xF0:
        case 0xF4:
        case 0xF5:
        case 0xF7:
        case 0xF9:
        case 0xFD: return 0;
        default: return 1;  // tune request and system real-time
    }
}

std::size_t convert_short(const Ump& ump, Midi1Messages& out) noexcept {
    const std::uint8_t size = midi1_message_size(ump.status());
    if (size == 0) { return 0; }
    out[0] = Midi1Message{.m_bytes = {ump.status(), low7(ump.data1()), low7(ump.data2())},
                          .m_size = size};
    for (std::size_t i = size; i < out[0].m_bytes.size(); ++i) { out[0].m_bytes[i] = 0; }
    return 1;
}

std::size_t convert_midi2(const Ump& ump, Midi1Messages& out) noexcept {
    const std::uint8_t channel = ump.channel();
    const std::uint32_t data = ump.m_words[1];
    const auto status = [channel](ChannelVoiceOpcode opcode) {
        return static_cast<std::uint8_t>((static_cast<std::uint8_t>(opcode) << 4U) | channel);
    };

    switch (ump.opcode()) {
        case ChannelVoiceOpcode::NoteOff:
            out[0] =
                message(status(ChannelVoiceOpcode::NoteOff), low7(ump.data1()), low7(data >> 25U));
            return 1;
        case ChannelVoiceOpcode::NoteOn: {
            // In MIDI 2.0 a note-on with velocity 0 is a note-on; in MIDI 1.0 it would
            // read as a note-off, so the smallest velocity is 1.
            std::uint8_t velocity = low7(data >> 25U);
            if (velocity == 0) { velocity = 1; }
            out[0] = message(status(ChannelVoiceOpcode::NoteOn), low7(ump.data1()), velocity);
            return 1;
        }
        case ChannelVoiceOpcode::PolyPressure:
            out[0] = message(status(ChannelVoiceOpcode::PolyPressure),
                             low7(ump.data1()),
                             low7(data >> 25U));
            return 1;
        case ChannelVoiceOpcode::ControlChange:
            out[0] = message(status(ChannelVoiceOpcode::ControlChange),
                             low7(ump.data1()),
                             low7(data >> 25U));
            return 1;
        case ChannelVoiceOpcode::ProgramChange: {
            std::size_t count = 0;
            const bool bank_valid = (ump.data2() & 0x1U) != 0;
            if (bank_valid) {
                const auto cc = static_cast<std::uint8_t>(k_status_control_change | channel);
                out[count++] = message(cc, k_cc_bank_msb, low7(data >> 8U));
                out[count++] = message(cc, k_cc_bank_lsb, low7(data));
            }
            out[count++] = message(static_cast<std::uint8_t>(k_status_program_change | channel),
                                   low7(data >> 24U));
            return count;
        }
        case ChannelVoiceOpcode::ChannelPressure:
            out[0] = message(status(ChannelVoiceOpcode::ChannelPressure), low7(data >> 25U));
            return 1;
        case ChannelVoiceOpcode::PitchBend:
            out[0] = message(status(ChannelVoiceOpcode::PitchBend),
                             low7(data >> 18U),
                             low7(data >> 25U));
            return 1;
        case ChannelVoiceOpcode::RegisteredController:
        case ChannelVoiceOpcode::AssignableController: {
            const bool registered = ump.opcode() == ChannelVoiceOpcode::RegisteredController;
            const auto cc = static_cast<std::uint8_t>(k_status_control_change | channel);
            out[0] = message(cc, registered ? k_cc_rpn_msb : k_cc_nrpn_msb, low7(ump.data1()));
            out[1] = message(cc, registered ? k_cc_rpn_lsb : k_cc_nrpn_lsb, low7(ump.data2()));
            out[2] = message(cc, k_cc_data_entry_msb, low7(data >> 25U));
            out[3] = message(cc, k_cc_data_entry_lsb, low7(data >> 18U));
            return 4;
        }
        case ChannelVoiceOpcode::RegisteredPerNoteController:
        case ChannelVoiceOpcode::AssignablePerNoteController:
        case ChannelVoiceOpcode::RelativeRegisteredController:
        case ChannelVoiceOpcode::RelativeAssignableController:
        case ChannelVoiceOpcode::PerNotePitchBend:
        case ChannelVoiceOpcode::PerNoteManagement: return 0;
    }
    return 0;  // opcode 0x7 is reserved
}

}  // namespace

std::size_t convert_to_midi1(const Ump& ump, Midi1Messages& out) noexcept {
    switch (ump.message_type()) {
        case MessageType::System:
        case MessageType::Midi1ChannelVoice: return convert_short(ump, out);
        case MessageType::Midi2ChannelVoice: return convert_midi2(ump, out);
        case MessageType::Utility:
        case MessageType::SysEx7:
        case MessageType::Data128:
        case MessageType::FlexData:
        case MessageType::Stream: return 0;
    }
    return 0;  // reserved message types
}

}  // namespace thl::inline THL_ABI_NAMESPACE::midi
