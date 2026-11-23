#pragma once

#include <tanh/core/AbiNamespace.h>
#include <tanh/midi/Ump.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <cstddef>
#include <cstdint>

namespace thl::inline THL_ABI_NAMESPACE::midi {

/// Reassembles Universal MIDI Packets from a byte stream in network byte order (each
/// 32-bit word big-endian) — the form in which Android hands over UMP devices.
///
/// Bytes may be split anywhere across calls. One reader per source; allocation-free and
/// non-blocking; not thread-safe. The stream has no framing of its own, so a reader that
/// has lost alignment only recovers through reset().
class UmpByteReader {
public:
    /// Feeds one byte. Returns true when it completed a packet, written to `out`.
    [[nodiscard]] bool feed(std::uint8_t byte, Ump& out) noexcept TANH_NONBLOCKING_FUNCTION {
        m_word = (m_word << 8U) | byte;
        if (++m_byte_count < 4) { return false; }
        m_byte_count = 0;

        if (m_word_count == 0) {
            m_packet = Ump{};
            m_words_needed = ump_word_count(m_word);
        }
        m_packet.m_words[m_word_count++] = m_word;
        m_word = 0;
        if (m_word_count < m_words_needed) { return false; }

        m_packet.m_size = m_words_needed;
        m_word_count = 0;
        out = m_packet;
        return true;
    }

    /// Feeds `size` bytes and calls `sink(const Ump&)` for every completed packet.
    template <typename Sink>
    void parse(const std::uint8_t* data, std::size_t size, Sink&& sink) {
        Ump ump;
        for (std::size_t i = 0; i < size; ++i) {
            if (feed(data[i], ump)) { sink(static_cast<const Ump&>(ump)); }
        }
    }

    /// Drops any partial packet, e.g. when the source is reopened.
    void reset() noexcept TANH_NONBLOCKING_FUNCTION {
        m_word = 0;
        m_byte_count = 0;
        m_word_count = 0;
        m_words_needed = 0;
    }

private:
    Ump m_packet;
    std::uint32_t m_word = 0;
    std::uint8_t m_byte_count = 0;
    std::uint8_t m_word_count = 0;
    std::uint8_t m_words_needed = 0;
};

}  // namespace thl::inline THL_ABI_NAMESPACE::midi
