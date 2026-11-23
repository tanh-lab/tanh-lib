#include <tanh/core/AbiNamespace.h>
#include <tanh/midi/Midi1StreamParser.h>
#include <tanh/midi/Ump.h>

#include <cstddef>
#include <cstdint>

namespace thl::inline THL_ABI_NAMESPACE::midi {

namespace {

constexpr std::uint8_t k_sysex_start = 0xF0;
constexpr std::uint8_t k_time_code = 0xF1;
constexpr std::uint8_t k_song_position = 0xF2;
constexpr std::uint8_t k_song_select = 0xF3;
constexpr std::uint8_t k_tune_request = 0xF6;
constexpr std::uint8_t k_first_real_time = 0xF8;
constexpr std::uint8_t k_undefined_real_time_1 = 0xF9;
constexpr std::uint8_t k_undefined_real_time_2 = 0xFD;
constexpr std::uint8_t k_sysex_chunk = 6;

}  // namespace

std::uint8_t Midi1StreamParser::feed(std::uint8_t byte, Output& out) noexcept {
    std::uint8_t count = 0;

    // System real-time: one byte, allowed anywhere, touches no parser state.
    if (byte >= k_first_real_time) {
        if (byte != k_undefined_real_time_1 && byte != k_undefined_real_time_2) {
            out[count++] = make_system(m_group, byte);
        }
        return count;
    }

    if (byte >= 0x80) {
        // Any status byte ends an open System Exclusive; 0xF7 is merely the usual one.
        if (m_in_sysex) { out[count++] = take_sysex(true); }

        // ... and cancels a message still waiting for data bytes.
        m_status = 0;
        m_data_count = 0;
        m_data_needed = 0;

        if (byte < k_sysex_start) {
            // Channel voice: program change and channel pressure take one data byte.
            m_status = byte;
            m_data_needed = (byte & 0xE0U) == 0xC0U ? 1 : 2;
            return count;
        }

        switch (byte) {
            case k_sysex_start: m_in_sysex = true; break;
            case k_time_code:
            case k_song_select:
                m_status = byte;
                m_data_needed = 1;
                break;
            case k_song_position:
                m_status = byte;
                m_data_needed = 2;
                break;
            case k_tune_request: out[count++] = make_system(m_group, byte); break;
            default: break;  // 0xF4 / 0xF5 are undefined; 0xF7 was handled above
        }
        return count;
    }

    // Data byte.
    if (m_in_sysex) {
        // A full chunk only goes out once another byte proves it is not the last one.
        if (m_sysex_count == k_sysex_chunk) { out[count++] = take_sysex(false); }
        m_sysex_bytes[m_sysex_count++] = byte;
        return count;
    }

    if (m_status == 0) { return count; }  // no status to attach it to

    m_data[m_data_count++] = byte;
    if (m_data_count < m_data_needed) { return count; }

    const std::uint8_t data2 = m_data_needed == 2 ? m_data[1] : std::uint8_t{0};
    m_data_count = 0;
    if (m_status < k_sysex_start) {
        // Running status: the status stays valid for the next data bytes.
        out[count++] = make_midi1_channel_voice(m_group, m_status, m_data[0], data2);
    } else {
        // System common messages have no running status.
        out[count++] = make_system(m_group, m_status, m_data[0], data2);
        m_status = 0;
        m_data_needed = 0;
    }
    return count;
}

void Midi1StreamParser::reset() noexcept {
    m_status = 0;
    m_data_count = 0;
    m_data_needed = 0;
    m_sysex_count = 0;
    m_in_sysex = false;
    m_sysex_started = false;
}

Ump Midi1StreamParser::take_sysex(bool final) noexcept {
    SysEx7Status status = SysEx7Status::Complete;
    if (final) {
        status = m_sysex_started ? SysEx7Status::End : SysEx7Status::Complete;
    } else {
        status = m_sysex_started ? SysEx7Status::Continue : SysEx7Status::Start;
    }

    for (std::size_t i = m_sysex_count; i < m_sysex_bytes.size(); ++i) { m_sysex_bytes[i] = 0; }
    const Ump ump = make_sysex7(m_group, status, m_sysex_bytes, m_sysex_count);

    m_sysex_count = 0;
    m_sysex_started = !final;
    if (final) { m_in_sysex = false; }
    return ump;
}

}  // namespace thl::inline THL_ABI_NAMESPACE::midi
