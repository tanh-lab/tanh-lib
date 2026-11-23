#pragma once

#include <tanh/core/AbiNamespace.h>
#include <tanh/midi/InputQueue.h>
#include <tanh/midi/Midi1StreamParser.h>
#include <tanh/midi/Ump.h>
#include <tanh/midi/UmpByteReader.h>

#include <cstddef>
#include <cstdint>

#include "DeviceBackend.h"

namespace thl::inline THL_ABI_NAMESPACE::midi {

/// One port of a device that delivers bytes (Android): turns what arrives into packets and
/// puts the accepted ones into the queue. The bytes are either a MIDI 1.0 stream or, for a
/// device of the Universal MIDI Packet transport, UMP words in network byte order. Holds
/// the state of that one stream; not thread-safe.
class PortReader {
public:
    explicit PortReader(bool ump) noexcept : m_ump(ump) {}

    void read(const std::uint8_t* data,
              std::size_t size,
              std::int64_t time_ns,
              std::uint16_t source,
              bool receive_sysex,
              InputQueue& queue) {
        const auto sink = [&](const Ump& ump) {
            if (accepts_packet(ump, receive_sysex)) { queue.push(ump, time_ns, source); }
        };
        if (m_ump) {
            m_ump_reader.parse(data, size, sink);
        } else {
            m_parser.parse(data, size, sink);
        }
    }

    /// Forgets partial messages and running status, when the port is opened again.
    void reset() noexcept {
        m_parser.reset();
        m_ump_reader.reset();
    }

private:
    Midi1StreamParser m_parser;
    UmpByteReader m_ump_reader;
    bool m_ump;
};

}  // namespace thl::inline THL_ABI_NAMESPACE::midi
