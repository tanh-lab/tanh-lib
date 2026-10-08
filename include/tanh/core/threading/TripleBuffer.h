// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <tanh/utils/RealtimeSanitizer.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <type_traits>

namespace thl {

/**
 * @brief Wait-free single-producer, single-consumer "latest value" mailbox.
 *
 * The reader always gets the most recent complete value; values it did not
 * pick up are overwritten. Exactly one writer thread and one reader thread,
 * which may be the same. T must be trivially copyable.
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

    /// Publish a copy of @p value.
    void write(const T& value) TANH_NONBLOCKING_FUNCTION {
        m_slots[m_back].m_value = value;
        publish();
    }

    /// The writer's slot, to fill in place before publish(). Its content is a
    /// stale value from some earlier publication, not the last one written.
    [[nodiscard]] T& write_buffer() TANH_NONBLOCKING_FUNCTION { return m_slots[m_back].m_value; }

    /// Publish write_buffer().
    void publish() TANH_NONBLOCKING_FUNCTION {
        const auto old =
            m_middle.exchange(static_cast<uint8_t>(m_back | k_dirty), std::memory_order_acq_rel);
        m_back = static_cast<uint8_t>(old & k_index_mask);
    }

    /// True if a value was published since the last read() (a hint: the
    /// writer may publish right after).
    [[nodiscard]] bool has_new() const TANH_NONBLOCKING_FUNCTION {
        return (m_middle.load(std::memory_order_relaxed) & k_dirty) != 0;
    }

    /**
     * @brief Copy the latest value into @p out if one was published since the
     *        last read(). Returns false (and leaves @p out untouched) otherwise.
     */
    bool read(T& out) TANH_NONBLOCKING_FUNCTION {
        if (!update()) { return false; }
        out = m_slots[m_front].m_value;
        return true;
    }

    /**
     * @brief Take the latest publication (if any) into the reader's slot.
     * @return true if it is new. latest() then refers to it until the next call.
     */
    bool update() TANH_NONBLOCKING_FUNCTION {
        if ((m_middle.load(std::memory_order_relaxed) & k_dirty) == 0) { return false; }
        const auto old = m_middle.exchange(m_front, std::memory_order_acq_rel);
        m_front = static_cast<uint8_t>(old & k_index_mask);
        return true;
    }

    /// The reader's current value (the default/initial value before the first update()).
    [[nodiscard]] const T& latest() const TANH_NONBLOCKING_FUNCTION {
        return m_slots[m_front].m_value;
    }

private:
    // The classic triple buffer: the writer owns the back slot, the reader the
    // front slot, and the middle slot is exchanged through one atomic byte that
    // also carries a "new data" bit. Ownership moves by acquire / release on
    // that byte, so plain copies of T never race.
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
