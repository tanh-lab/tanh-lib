#pragma once

#include <tanh/core/AbiNamespace.h>
#include <tanh/core/threading/LockFreeQueue.h>
#include <tanh/midi/Ump.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace thl::inline THL_ABI_NAMESPACE::midi {

/// Carries MIDI from the threads it arrives on to the thread that processes it —
/// typically from the platform's MIDI receive threads to the audio thread.
///
/// Bounded and lock-free: push() and pop() neither allocate nor block, so both ends may
/// run on real-time threads. Any number of threads may push (each input has its own
/// receive thread on some platforms); packets of one pushing thread stay in order. One
/// thread drains.
///
/// When the queue is full the new packet is dropped and dropped() rises. A consumer that
/// sees the count change has missed messages — possibly note-offs — and should release
/// its sounding notes.
class InputQueue {
public:
    /// `capacity` packets, rounded up to a power of two. Allocates; construct it off the
    /// real-time threads.
    explicit InputQueue(std::size_t capacity = k_default_capacity) : m_queue(capacity) {}

    /// Enqueues a packet. Returns false, and counts a drop, when the queue is full.
    bool push(const TimedUmp& packet) noexcept TANH_NONBLOCKING_FUNCTION {
        if (m_queue.try_push(packet)) { return true; }
        m_dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    /// Enqueues `ump` with its arrival time and source slot.
    bool push(const Ump& ump,
              std::int64_t time_ns,
              std::uint16_t source = 0) noexcept TANH_NONBLOCKING_FUNCTION {
        return push(TimedUmp{.m_time_ns = time_ns, .m_ump = ump, .m_source = source});
    }

    /// Dequeues the oldest packet. Returns false when the queue is empty.
    [[nodiscard]] bool pop(TimedUmp& out) noexcept TANH_NONBLOCKING_FUNCTION {
        return m_queue.try_pop(out);
    }

    /// Calls `sink(const TimedUmp&)` for every queued packet, oldest first, and returns
    /// how many there were. Takes at most capacity() packets per call, so a producer that
    /// keeps pushing cannot hold the caller in the loop.
    template <typename Sink>
    std::size_t drain(Sink&& sink) {
        const std::size_t limit = m_queue.capacity();
        std::size_t count = 0;
        TimedUmp packet;
        while (count < limit && m_queue.try_pop(packet)) {
            sink(static_cast<const TimedUmp&>(packet));
            ++count;
        }
        return count;
    }

    /// Packets dropped because the queue was full, since construction (wraps at 2^32).
    [[nodiscard]] std::uint32_t dropped() const noexcept TANH_NONBLOCKING_FUNCTION {
        return m_dropped.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return m_queue.capacity(); }

    /// Default capacity: several seconds of dense controller data at audio block rates.
    static constexpr std::size_t k_default_capacity = 1024;

private:
    core::DynamicLockFreeQueue<TimedUmp> m_queue;
    std::atomic<std::uint32_t> m_dropped{0};
};

}  // namespace thl::inline THL_ABI_NAMESPACE::midi
