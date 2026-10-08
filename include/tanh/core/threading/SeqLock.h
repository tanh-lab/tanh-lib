// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <tanh/utils/RealtimeSanitizer.h>

#include <atomic>
#include <cstdint>
#include <optional>

namespace thl {

/**
 * @brief Sequence counter of a seqlock over data the caller keeps in atomics.
 *
 * Lets a reader see a batch of stores completely or not at all without ever
 * waiting: a reader that overlaps a write discards what it read (keeps its
 * previous copy) instead of retrying. The protected data must itself be
 * atomics (relaxed loads and stores are enough), so racing reads are not data
 * races.
 *
 * One writer at a time: the caller serialises writers (e.g. under a mutex).
 * Any number of readers.
 *
 * ```cpp
 * // Writer
 * lock.write_begin();
 * a.store(1, std::memory_order_relaxed);
 * b.store(2, std::memory_order_relaxed);
 * lock.write_end();
 *
 * // Reader (real-time safe)
 * if (const auto token = lock.read_begin()) {
 *     const int x = a.load(std::memory_order_relaxed);
 *     const int y = b.load(std::memory_order_relaxed);
 *     if (lock.read_valid(*token)) { use(x, y); }  // else keep the last copy
 * }
 * ```
 */
class SeqLock {
public:
    /// Writer: call before the stores of a batch (the count turns odd).
    void write_begin() noexcept {
        const uint32_t sequence = m_sequence.load(std::memory_order_relaxed);
        m_sequence.store(sequence + 1, std::memory_order_relaxed);
        // The batch's stores cannot become visible before the odd count.
        std::atomic_thread_fence(std::memory_order_release);
    }

    /// Writer: call after the stores (the count turns even and publishes them).
    void write_end() noexcept {
        const uint32_t sequence = m_sequence.load(std::memory_order_relaxed);
        m_sequence.store(sequence + 1, std::memory_order_release);
    }

    /// Reader: a token to check after the loads, or nullopt while a write is in progress.
    [[nodiscard]] std::optional<uint32_t> read_begin() const noexcept TANH_NONBLOCKING_FUNCTION {
        // acquire: an even count makes every store of the batch that published it visible.
        const uint32_t sequence = m_sequence.load(std::memory_order_acquire);
        if ((sequence & 1u) != 0) { return std::nullopt; }
        return sequence;
    }

    /// Reader: true if no write overlapped the loads since read_begin() returned @p token.
    [[nodiscard]] bool read_valid(uint32_t token) const noexcept TANH_NONBLOCKING_FUNCTION {
        // The loads cannot move after the check: if a writer's odd count is not
        // seen here, none of its stores was read either.
        std::atomic_thread_fence(std::memory_order_acquire);
        return m_sequence.load(std::memory_order_relaxed) == token;
    }

private:
    std::atomic<uint32_t> m_sequence{0};
};

}  // namespace thl
