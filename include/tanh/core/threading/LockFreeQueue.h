// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <tanh/utils/RealtimeSanitizer.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>

namespace thl::core {

/**
 * Bounded lock-free multi-producer / multi-consumer queue.
 *
 * Fixed capacity, no allocation after construction, no locks, no system
 * calls: every operation is a bounded number of atomic loads/stores and
 * CAS attempts, so both ends may run on real-time threads. Implements
 * Dmitry Vyukov's bounded MPMC scheme: each slot carries a sequence number
 * that tells producers and consumers whether it is theirs to use.
 *
 * Semantics:
 * - try_push() fails when the queue is full (the producer never waits).
 * - push_overwrite() makes room by discarding the oldest element instead,
 *   for "latest data wins" streams such as meters or log records.
 * - try_pop() fails when the queue is empty.
 * - Elements from one producer are popped in the order they were pushed;
 *   there is no ordering guarantee between different producers.
 *
 * T must be trivially copyable and default constructible; the queue copies
 * elements by value into pre-allocated slots. Capacity must be a power of
 * two. sizeof(LockFreeQueue) ~= Capacity * (sizeof(T) + 8) + 128, so large
 * queues belong at namespace scope or on the heap, not on the stack.
 *
 * Objects of this type have trivial destructors; a namespace-scope instance
 * therefore stays valid during static teardown.
 */
template <typename T, std::size_t Capacity>
class LockFreeQueue {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                  "LockFreeQueue capacity must be a power of two >= 2");
    static_assert(std::is_trivially_copyable_v<T>, "LockFreeQueue requires a trivially copyable T");
    static_assert(std::is_nothrow_default_constructible_v<T>,
                  "LockFreeQueue requires a nothrow default constructible T");

public:
    using value_type = T;

    LockFreeQueue() noexcept {
        for (std::size_t i = 0; i < Capacity; ++i) {
            m_cells[i].m_seq.store(i, std::memory_order_relaxed);
        }
    }

    LockFreeQueue(const LockFreeQueue&) = delete;
    LockFreeQueue& operator=(const LockFreeQueue&) = delete;
    LockFreeQueue(LockFreeQueue&&) = delete;
    LockFreeQueue& operator=(LockFreeQueue&&) = delete;
    ~LockFreeQueue() = default;

    static constexpr std::size_t capacity() noexcept { return Capacity; }

    /// Enqueue a copy of `item`. Returns false (and leaves the queue
    /// unchanged) if it is full.
    bool try_push(const T& item) noexcept TANH_NONBLOCKING_FUNCTION {
        std::size_t pos = m_enqueue_pos.load(std::memory_order_relaxed);
        Cell* cell = nullptr;
        for (;;) {
            cell = &m_cells[pos & k_mask];
            const std::size_t seq = cell->m_seq.load(std::memory_order_acquire);
            const auto diff = static_cast<std::ptrdiff_t>(seq) - static_cast<std::ptrdiff_t>(pos);
            if (diff == 0) {
                if (m_enqueue_pos.compare_exchange_weak(pos,
                                                        pos + 1,
                                                        std::memory_order_relaxed,
                                                        std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false;  // full
            } else {
                pos = m_enqueue_pos.load(std::memory_order_relaxed);
            }
        }
        cell->m_value = item;
        cell->m_seq.store(pos + 1, std::memory_order_release);
        return true;
    }

    /// Enqueue a copy of `item`, discarding the oldest element(s) if the
    /// queue is full. Returns the number of elements discarded. Under heavy
    /// contention the attempt is bounded, so in the rare case where room
    /// could not be made the item is dropped and `Capacity + 1` is returned.
    std::size_t push_overwrite(const T& item) noexcept TANH_NONBLOCKING_FUNCTION {
        std::size_t discarded = 0;
        for (std::size_t attempt = 0; attempt <= Capacity; ++attempt) {
            if (try_push(item)) { return discarded; }
            T victim;
            if (try_pop(victim)) { ++discarded; }
        }
        return Capacity + 1;
    }

    /// Dequeue into `out`. Returns false (and leaves `out` untouched) if
    /// the queue is empty.
    bool try_pop(T& out) noexcept TANH_NONBLOCKING_FUNCTION {
        std::size_t pos = m_dequeue_pos.load(std::memory_order_relaxed);
        Cell* cell = nullptr;
        for (;;) {
            cell = &m_cells[pos & k_mask];
            const std::size_t seq = cell->m_seq.load(std::memory_order_acquire);
            const auto diff =
                static_cast<std::ptrdiff_t>(seq) - static_cast<std::ptrdiff_t>(pos + 1);
            if (diff == 0) {
                if (m_dequeue_pos.compare_exchange_weak(pos,
                                                        pos + 1,
                                                        std::memory_order_relaxed,
                                                        std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false;  // empty
            } else {
                pos = m_dequeue_pos.load(std::memory_order_relaxed);
            }
        }
        out = cell->m_value;
        cell->m_seq.store(pos + k_mask + 1, std::memory_order_release);
        return true;
    }

    /// Approximate number of queued elements. Exact only when no other
    /// thread is pushing or popping concurrently.
    std::size_t size_approx() const noexcept TANH_NONBLOCKING_FUNCTION {
        const std::size_t head = m_dequeue_pos.load(std::memory_order_relaxed);
        const std::size_t tail = m_enqueue_pos.load(std::memory_order_relaxed);
        return tail >= head ? tail - head : 0;
    }

    bool empty_approx() const noexcept TANH_NONBLOCKING_FUNCTION { return size_approx() == 0; }

    /// Discard every queued element. Not thread-safe: callers must ensure
    /// no producer or consumer is active.
    void clear() noexcept {
        T discard;
        while (try_pop(discard)) {}
    }

private:
    static constexpr std::size_t k_mask = Capacity - 1;
    static constexpr std::size_t k_cache_line = 64;

    struct Cell {
        std::atomic<std::size_t> m_seq{0};
        T m_value{};
    };

    alignas(k_cache_line) std::atomic<std::size_t> m_enqueue_pos{0};
    alignas(k_cache_line) std::atomic<std::size_t> m_dequeue_pos{0};
    alignas(k_cache_line) std::array<Cell, Capacity> m_cells{};
};

}  // namespace thl::core
