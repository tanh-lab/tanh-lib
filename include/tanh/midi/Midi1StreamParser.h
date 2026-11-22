#pragma once

#include <tanh/core/AbiNamespace.h>
#include <tanh/core/Exports.h>
#include <tanh/midi/Ump.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace thl::inline THL_ABI_NAMESPACE::midi {

/// Turns a MIDI 1.0 byte stream into Universal MIDI Packets.
///
/// The input is what a serial MIDI source delivers (USB and Bluetooth MIDI on Android, a
/// plugin host's MIDI buffer): messages may be split anywhere across calls, several may
/// arrive in one call, running status is allowed and system real-time bytes may sit in the
/// middle of another message. The output keeps MIDI 1.0 resolution: channel voice messages
/// become type 2, system common and real-time type 1, System Exclusive type 3 packets of
/// up to six bytes each. Nothing is scaled or interpreted — a note-on with velocity 0
/// stays one.
///
/// One parser per source: it holds the running status and any partial message of that
/// stream. Allocation-free and non-blocking; not thread-safe.
class TANH_API Midi1StreamParser {
public:
    /// The packets one byte can complete: at most two (a status byte that ends an open
    /// System Exclusive and is a complete message itself).
    using Output = std::array<Ump, 2>;

    /// `group` (0 to 15) is stamped into every packet.
    explicit Midi1StreamParser(std::uint8_t group = 0) noexcept
        : m_group(static_cast<std::uint8_t>(group & 0xFU)) {}

    /// Feeds one byte. Returns how many packets it completed (0 to 2), written to the
    /// front of `out`.
    [[nodiscard]] std::uint8_t feed(std::uint8_t byte,
                                    Output& out) noexcept TANH_NONBLOCKING_FUNCTION;

    /// Feeds `size` bytes and calls `sink(const Ump&)` for every completed packet, in
    /// order.
    template <typename Sink>
    void parse(const std::uint8_t* data, std::size_t size, Sink&& sink) {
        Output out;
        for (std::size_t i = 0; i < size; ++i) {
            const std::uint8_t count = feed(data[i], out);
            for (std::uint8_t k = 0; k < count; ++k) { sink(static_cast<const Ump&>(out[k])); }
        }
    }

    /// Forgets the running status and any partial message, e.g. when the source is
    /// reopened.
    void reset() noexcept TANH_NONBLOCKING_FUNCTION;

    [[nodiscard]] std::uint8_t group() const noexcept { return m_group; }

private:
    [[nodiscard]] Ump take_sysex(bool final) noexcept;

    std::array<std::uint8_t, 6> m_sysex_bytes{};
    std::array<std::uint8_t, 2> m_data{};
    std::uint8_t m_group;
    std::uint8_t m_status = 0;       // status awaiting data bytes; 0 = none
    std::uint8_t m_data_count = 0;   // data bytes collected for m_status
    std::uint8_t m_data_needed = 0;  // data bytes m_status takes
    std::uint8_t m_sysex_count = 0;  // bytes waiting in m_sysex_bytes
    bool m_in_sysex = false;
    bool m_sysex_started = false;  // a Start packet of the open SysEx has gone out
};

}  // namespace thl::inline THL_ABI_NAMESPACE::midi
