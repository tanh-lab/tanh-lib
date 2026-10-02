// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <tanh/utils/RealtimeSanitizer.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <type_traits>

namespace thl {

/**
 * @brief Wait-free single-producer / single-consumer "latest value" mailbox.
 *
 * Three slots of T: the writer owns one (back), the reader owns one (front),
 * and the third (middle) is exchanged through one atomic byte that also
 * carries a "new data" bit. Neither side ever waits or retries:
 *
 * - write() / publish() copy into the back slot and swap it with the middle
 *   one (one atomic exchange, release).
 * - read() swaps the middle slot into front if it is newer (one atomic
 *   exchange, acquire) and copies it out.
 *
 * The reader always gets the most recent complete value; intermediate values
 * it did not pick up are overwritten (no queue). A slot is only ever touched by
 * the side that currently owns it, and ownership moves through acquire/release
 * on the index byte, so plain copies of T are race-free (TSan-clean).
 *
 * Exactly one writer thread and one reader thread (they may be the same). Use
 * one buffer per reader; a second reader must read through the first one's
 * copy. T must be trivially copyable; each slot sits on its own cache line.
 *
 * The algorithm is the classic triple buffer (see e.g. HadrienG2/triple-buffer).
 */
template <typename T>
class TripleBuffer {
    static_assert(std::is_trivially_copyable_v<T>, "TripleBuffer requires a trivially copyable T");
    static_assert(std::is_default_constructible_v<T>,
                  "TripleBuffer requires a default constructible T");

public:
    TripleBuffer() = default;
    explicit TripleBuffer(const T& initial) {
        for (auto& s : m_slots) { s.m_value = initial; }
    }

    TripleBuffer(const TripleBuffer&) = delete;
    TripleBuffer& operator=(const TripleBuffer&) = delete;
    TripleBuffer(TripleBuffer&&) = delete;
    TripleBuffer& operator=(TripleBuffer&&) = delete;
    ~TripleBuffer() = default;

    // ── Writer ─────────────────────────────────────────────────────────────

    /// Publish a copy of @p value.
    void write(const T& value) noexcept TANH_NONBLOCKING_FUNCTION {
        m_slots[m_back].m_value = value;
        publish();
    }

    /// The writer's slot, to fill in place before publish(). Its content is a
    /// stale value from some earlier publication, not the last one written.
    [[nodiscard]] T& write_buffer() noexcept TANH_NONBLOCKING_FUNCTION {
        return m_slots[m_back].m_value;
    }

    /// Publish write_buffer().
    void publish() noexcept TANH_NONBLOCKING_FUNCTION {
        const auto old =
            m_middle.exchange(static_cast<uint8_t>(m_back | k_dirty), std::memory_order_acq_rel);
        m_back = static_cast<uint8_t>(old & k_index_mask);
    }

    // ── Reader ─────────────────────────────────────────────────────────────

    /// True if a value was published since the last read() (a hint: the
    /// writer may publish right after).
    [[nodiscard]] bool has_new() const noexcept TANH_NONBLOCKING_FUNCTION {
        return (m_middle.load(std::memory_order_relaxed) & k_dirty) != 0;
    }

    /**
     * @brief Copy the latest value into @p out if one was published since the
     *        last read(). Returns false (and leaves @p out untouched) otherwise.
     */
    bool read(T& out) noexcept TANH_NONBLOCKING_FUNCTION {
        if (!update()) { return false; }
        out = m_slots[m_front].m_value;
        return true;
    }

    /**
     * @brief Take the latest publication (if any) into the reader's slot.
     * @return true if it is new. latest() then refers to it until the next call.
     */
    bool update() noexcept TANH_NONBLOCKING_FUNCTION {
        if ((m_middle.load(std::memory_order_relaxed) & k_dirty) == 0) { return false; }
        const auto old = m_middle.exchange(m_front, std::memory_order_acq_rel);
        m_front = static_cast<uint8_t>(old & k_index_mask);
        return true;
    }

    /// The reader's current value (the default/initial value before the first update()).
    [[nodiscard]] const T& latest() const noexcept TANH_NONBLOCKING_FUNCTION {
        return m_slots[m_front].m_value;
    }

private:
    static constexpr uint8_t k_index_mask = 0x3;
    static constexpr uint8_t k_dirty = 0x4;

    struct alignas(64) Slot {
        T m_value{};
    };

    std::array<Slot, 3> m_slots{};
    alignas(64) std::atomic<uint8_t> m_middle{1};
    alignas(64) uint8_t m_back = 0;   // writer only
    alignas(64) uint8_t m_front = 2;  // reader only
};

}  // namespace thl
