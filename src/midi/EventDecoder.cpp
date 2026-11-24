#include <tanh/core/AbiNamespace.h>
#include <tanh/midi/Event.h>
#include <tanh/midi/EventDecoder.h>
#include <tanh/midi/Ump.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace thl::inline THL_ABI_NAMESPACE::midi {

namespace {

constexpr std::uint8_t k_cc_data_entry_msb = 6;
constexpr std::uint8_t k_cc_data_entry_lsb = 38;
constexpr std::uint8_t k_cc_nrpn_lsb = 98;
constexpr std::uint8_t k_cc_nrpn_msb = 99;
constexpr std::uint8_t k_cc_rpn_lsb = 100;
constexpr std::uint8_t k_cc_rpn_msb = 101;

constexpr std::uint8_t k_attribute_pitch_7_9 = 3;
constexpr std::uint8_t k_per_note_pitch_7_25 = 3;
constexpr std::uint8_t k_per_note_brightness = 74;

float unipolar7(std::uint32_t value) noexcept {
    return static_cast<float>(value) / 127.0f;
}

// RPN / NRPN data entry, MSB * 128 + LSB. A controller that sends the MSB alone tops out
// at (127, 0), so that is the value 1; the LSB refines within one MSB step and the few
// values above (127, 0) are 1 as well.
float parameter_value(std::uint32_t value) noexcept {
    return std::min(static_cast<float>(value) / (127.0f * 128.0f), 1.0f);
}

float unipolar16(std::uint32_t value) noexcept {
    return static_cast<float>(value) / 65535.0f;
}

float unipolar32(std::uint32_t value) noexcept {
    return static_cast<float>(static_cast<double>(value) / 4294967295.0);
}

// Pitch bend: the centre is exactly 0, both ends exactly -1 and 1 (the upper half has one
// step less than the lower).
float bipolar14(std::uint32_t value) noexcept {
    constexpr float k_centre = 8192.0f;
    const float offset = static_cast<float>(value) - k_centre;
    return offset < 0.0f ? offset / k_centre : offset / (k_centre - 1.0f);
}

float bipolar32(std::uint32_t value) noexcept {
    constexpr double k_centre = 2147483648.0;
    const double offset = static_cast<double>(value) - k_centre;
    return static_cast<float>(offset < 0.0 ? offset / k_centre : offset / (k_centre - 1.0));
}

void set_value7(Event& event, std::uint32_t value) noexcept {
    event.m_value = unipolar7(value);
    event.m_data = value << 25U;
}

void set_value32(Event& event, std::uint32_t value) noexcept {
    event.m_value = unipolar32(value);
    event.m_data = value;
}

void set_expression(Event& event, Expression expression) noexcept {
    event.m_type = EventType::NoteExpression;
    event.m_index = static_cast<std::uint16_t>(expression);
}

}  // namespace

void EventDecoder::decode(const Ump& ump,
                          std::uint32_t sample_offset,
                          std::uint16_t source,
                          EventWriter& out) noexcept TANH_NONBLOCKING_FUNCTION {
    Event event;
    event.m_sample_offset = sample_offset;
    event.m_source = source;
    event.m_group = ump.group();
    event.m_channel = ump.channel();

    switch (ump.message_type()) {
        case MessageType::Midi1ChannelVoice: decode_midi1(ump, event, out); break;
        case MessageType::Midi2ChannelVoice: decode_midi2(ump, event, out); break;
        default: break;
    }
}

void EventDecoder::reset() noexcept TANH_NONBLOCKING_FUNCTION {
    m_parameters.fill(ParameterSelection{});
}

void EventDecoder::decode_midi1(const Ump& ump, Event event, EventWriter& out) noexcept {
    const std::uint32_t data1 = ump.data1() & 0x7FU;
    const std::uint32_t data2 = ump.data2() & 0x7FU;

    switch (ump.opcode()) {
        case ChannelVoiceOpcode::NoteOff:
        case ChannelVoiceOpcode::NoteOn: {
            const bool on = ump.opcode() == ChannelVoiceOpcode::NoteOn && data2 != 0;
            event.m_type = on ? EventType::NoteOn : EventType::NoteOff;
            event.m_key = static_cast<std::uint8_t>(data1);
            set_value7(event, data2);
            out.push(event);
            break;
        }
        case ChannelVoiceOpcode::PolyPressure:
            set_expression(event, Expression::Pressure);
            event.m_key = static_cast<std::uint8_t>(data1);
            set_value7(event, data2);
            out.push(event);
            break;
        case ChannelVoiceOpcode::ControlChange:
            if (decode_parameter(static_cast<std::uint8_t>(data1),
                                 static_cast<std::uint8_t>(data2),
                                 event,
                                 out)) {
                break;
            }
            event.m_type = EventType::Controller;
            event.m_index = static_cast<std::uint16_t>(data1);
            set_value7(event, data2);
            out.push(event);
            break;
        case ChannelVoiceOpcode::ProgramChange:
            event.m_type = EventType::ProgramChange;
            event.m_index = static_cast<std::uint16_t>(data1);
            out.push(event);
            break;
        case ChannelVoiceOpcode::ChannelPressure:
            event.m_type = EventType::ChannelPressure;
            set_value7(event, data1);
            out.push(event);
            break;
        case ChannelVoiceOpcode::PitchBend: {
            const std::uint32_t value = data1 | (data2 << 7U);
            event.m_type = EventType::PitchBend;
            event.m_value = bipolar14(value);
            event.m_data = value << 18U;
            out.push(event);
            break;
        }
        default: break;
    }
}

bool EventDecoder::decode_parameter(std::uint8_t controller,
                                    std::uint8_t value,
                                    Event event,
                                    EventWriter& out) noexcept {
    auto& selection =
        m_parameters[(static_cast<std::size_t>(event.m_group) << 4U) | event.m_channel];

    switch (controller) {
        case k_cc_rpn_msb:
            selection.m_has_data_msb = false;
            selection.m_msb = value;
            selection.m_registered = true;
            return true;
        case k_cc_rpn_lsb:
            selection.m_has_data_msb = false;
            selection.m_lsb = value;
            selection.m_registered = true;
            return true;
        case k_cc_nrpn_msb:
            selection.m_has_data_msb = false;
            selection.m_msb = value;
            selection.m_registered = false;
            return true;
        case k_cc_nrpn_lsb:
            selection.m_has_data_msb = false;
            selection.m_lsb = value;
            selection.m_registered = false;
            return true;
        case k_cc_data_entry_msb:
        case k_cc_data_entry_lsb: {
            if (!selection.selected()) { return false; }
            std::uint32_t data = 0;
            if (controller == k_cc_data_entry_msb) {
                selection.m_data_msb = value;
                selection.m_has_data_msb = true;
                data = static_cast<std::uint32_t>(value) << 7U;
            } else if (!selection.m_has_data_msb) {
                // An LSB belongs to the MSB before it; without one it says nothing.
                return true;
            } else {
                data = (static_cast<std::uint32_t>(selection.m_data_msb) << 7U) | value;
            }
            event.m_type = selection.m_registered ? EventType::RegisteredController
                                                  : EventType::AssignableController;
            event.m_index = static_cast<std::uint16_t>(
                (static_cast<std::uint16_t>(selection.m_msb) << 7U) | selection.m_lsb);
            event.m_value = parameter_value(data);
            event.m_data = data << 18U;
            out.push(event);
            return true;
        }
        default: return false;
    }
}

void EventDecoder::decode_midi2(const Ump& ump, Event event, EventWriter& out) noexcept {
    const std::uint8_t index = ump.data1();
    const std::uint8_t extra = ump.data2();
    const std::uint32_t data = ump.m_words[1];

    switch (ump.opcode()) {
        case ChannelVoiceOpcode::NoteOff:
        case ChannelVoiceOpcode::NoteOn: {
            const bool on = ump.opcode() == ChannelVoiceOpcode::NoteOn;
            const std::uint32_t velocity = data >> 16U;
            event.m_type = on ? EventType::NoteOn : EventType::NoteOff;
            event.m_key = index & 0x7FU;
            event.m_value = unipolar16(velocity);
            event.m_data = velocity << 16U;
            out.push(event);
            if (on && extra == k_attribute_pitch_7_9) {
                // Q7.9: semitones in the top 7 of 16 bits.
                const std::uint32_t pitch = data & 0xFFFFU;
                set_expression(event, Expression::Pitch);
                event.m_value = static_cast<float>(pitch) / 512.0f;
                event.m_data = pitch << 16U;
                out.push(event);
            }
            break;
        }
        case ChannelVoiceOpcode::PolyPressure:
            set_expression(event, Expression::Pressure);
            event.m_key = index & 0x7FU;
            set_value32(event, data);
            out.push(event);
            break;
        case ChannelVoiceOpcode::RegisteredPerNoteController:
            event.m_key = index & 0x7FU;
            if (extra == k_per_note_brightness) {
                set_expression(event, Expression::Timbre);
                set_value32(event, data);
                out.push(event);
            } else if (extra == k_per_note_pitch_7_25) {
                // Q7.25: semitones in the top 7 of 32 bits.
                set_expression(event, Expression::Pitch);
                event.m_value = static_cast<float>(static_cast<double>(data) / 33554432.0);
                event.m_data = data;
                out.push(event);
            }
            break;
        case ChannelVoiceOpcode::PerNotePitchBend:
            set_expression(event, Expression::PitchBend);
            event.m_key = index & 0x7FU;
            event.m_value = bipolar32(data);
            event.m_data = data;
            out.push(event);
            break;
        case ChannelVoiceOpcode::RegisteredController:
        case ChannelVoiceOpcode::AssignableController:
            event.m_type = ump.opcode() == ChannelVoiceOpcode::RegisteredController
                               ? EventType::RegisteredController
                               : EventType::AssignableController;
            event.m_index = static_cast<std::uint16_t>(((index & 0x7FU) << 7U) | (extra & 0x7FU));
            set_value32(event, data);
            out.push(event);
            break;
        case ChannelVoiceOpcode::ControlChange:
            event.m_type = EventType::Controller;
            event.m_index = index & 0x7FU;
            set_value32(event, data);
            out.push(event);
            break;
        case ChannelVoiceOpcode::ProgramChange:
            event.m_type = EventType::ProgramChange;
            event.m_index = static_cast<std::uint16_t>((data >> 24U) & 0x7FU);
            out.push(event);
            break;
        case ChannelVoiceOpcode::ChannelPressure:
            event.m_type = EventType::ChannelPressure;
            set_value32(event, data);
            out.push(event);
            break;
        case ChannelVoiceOpcode::PitchBend:
            event.m_type = EventType::PitchBend;
            event.m_value = bipolar32(data);
            event.m_data = data;
            out.push(event);
            break;
        default: break;
    }
}

}  // namespace thl::inline THL_ABI_NAMESPACE::midi
