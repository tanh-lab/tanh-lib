// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <tanh/core/AbiNamespace.h>
#include <tanh/core/threading/CacheLine.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cassert>
#include <cstddef>
#include <limits>
#include <span>
#include <type_traits>
#include <vector>

namespace thl::inline THL_ABI_NAMESPACE::core {

/**
 * A multi-channel lock-free single-producer / single-consumer ring of samples: one thread writes
 * blocks (the audio thread), one other thread reads them (a UI or analysis thread).
 *
 * Semantics:
 * - write() copies as much of a block as fits and drops the rest, counted in dropped(): the
 *   writer never waits and never touches what the reader may be copying. A reader that keeps up
 *   never sees a drop; one that wants only the latest samples skips the rest (skip(),
 *   read_latest()).
 * - read() copies what has arrived, oldest first, up to the destination's length.
 * - Every channel moves together: one write position, one read position.
 *
 * Threads: prepare() allocates and must not run concurrently with anything else. write() and
 * free_space() belong to the writer, read(), read_latest(), skip() and available() to the
 * reader; all are real-time safe (no allocation, no locks: two atomic indices). dropped() may be
 * called from any thread.
 *
 * Memory ordering: the writer copies samples, then publishes its position with a release store;
 * the reader acquires it before copying, and publishes its own position with a release store
 * after copying, which the writer acquires before reusing that space. The positions only grow
 * (modulo 2^N), so full and empty are told apart without a spare slot.
 *
 * T must be trivially copyable; the capacity is rounded up to a power of two.
 *
 * Why not moodycamel (readerwriterqueue, concurrentqueue), already fetched for the modulation
 * component:
 * - ReaderWriterQueue (SPSC) moves one element per call: a call per sample per channel, or a
 *   fixed-size block struct per entry that wastes shorter blocks and splits longer ones. It has
 *   no bulk enqueue.
 * - ConcurrentQueue has try_enqueue_bulk(), but it is multi-producer / multi-consumer (more than
 *   one writer and one reader need), and only its try_ variants with preallocated capacity never
 *   allocate.
 * - Both queue elements, not channels: planar blocks would be interleaved on the way in, or need
 *   a queue per channel that can drift apart.
 * - CMakeLists.txt keeps moodycamel inside the modulation component; core would make it a public
 *   dependency of the library.
 * This ring moves whole multi-channel blocks with two copies per channel, drops and counts what
 * does not fit, and needs no dependency.
 */
template <typename T>
class SpscRingBuffer {
    static_assert(std::is_trivially_copyable_v<T>,
                  "SpscRingBuffer requires a trivially copyable T");

public:
    SpscRingBuffer() = default;
    SpscRingBuffer(const SpscRingBuffer&) = delete;
    SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;

    /// Allocates @p num_channels channels of at least @p capacity samples each and empties the
    /// ring. Not concurrent with any other call.
    void prepare(size_t num_channels, size_t capacity) {
        assert(capacity <= (std::numeric_limits<size_t>::max() / 2) + 1 &&
               "SpscRingBuffer::prepare(): capacity too large to round up to a power of two");
        assert((num_channels == 0 || std::bit_ceil(std::max<size_t>(capacity, 1)) <=
                                         std::numeric_limits<size_t>::max() / num_channels) &&
               "SpscRingBuffer::prepare(): num_channels * capacity overflows");
        m_num_channels = num_channels;
        m_capacity = std::bit_ceil(std::max<size_t>(capacity, 1));
        m_mask = m_capacity - 1;
        m_samples.assign(m_num_channels * m_capacity, T{});
        m_write.store(0, std::memory_order_relaxed);
        m_read.store(0, std::memory_order_relaxed);
        m_dropped.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] size_t num_channels() const { return m_num_channels; }
    [[nodiscard]] size_t capacity() const { return m_capacity; }

    // ---- writer ----------------------------------------------------------------------------

    /// Writer: the samples that fit now.
    [[nodiscard]] size_t free_space() const TANH_NONBLOCKING_FUNCTION {
        return m_capacity -
               (m_write.load(std::memory_order_relaxed) - m_read.load(std::memory_order_acquire));
    }

    /// Writer: appends @p num_samples of every channel (@p channels[c]: channel c; channels
    /// beyond @p channels.size() get T{}, extra ones are ignored). Returns the samples written;
    /// the rest are dropped and counted.
    size_t write(std::span<const T* const> channels, size_t num_samples) TANH_NONBLOCKING_FUNCTION {
        const size_t write = m_write.load(std::memory_order_relaxed);
        const size_t free = m_capacity - (write - m_read.load(std::memory_order_acquire));
        const size_t count = std::min(num_samples, free);
        if (count < num_samples) {
            // The one writer is the only one to change it: no read-modify-write needed.
            m_dropped.store(m_dropped.load(std::memory_order_relaxed) + (num_samples - count),
                            std::memory_order_relaxed);
        }
        if (count == 0) { return 0; }
        const size_t start = write & m_mask;
        const size_t first = std::min(count, m_capacity - start);
        for (size_t ch = 0; ch < m_num_channels; ++ch) {
            T* ring = channel(ch);
            if (ch < channels.size() && channels[ch] != nullptr) {
                std::copy_n(channels[ch], first, ring + start);
                std::copy_n(channels[ch] + first, count - first, ring);
            } else {
                std::fill_n(ring + start, first, T{});
                std::fill_n(ring, count - first, T{});
            }
        }
        m_write.store(write + count, std::memory_order_release);
        return count;
    }

    // ---- reader ----------------------------------------------------------------------------

    /// Reader: the samples that have arrived and not been read.
    [[nodiscard]] size_t available() const TANH_NONBLOCKING_FUNCTION {
        return m_write.load(std::memory_order_acquire) - m_read.load(std::memory_order_relaxed);
    }

    /// Reader: copies up to @p num_samples of the oldest arrived samples into @p channels
    /// (@p channels[c]: channel c; a null or missing destination skips that channel) and
    /// consumes them. Returns the samples read.
    size_t read(std::span<T* const> channels, size_t num_samples) TANH_NONBLOCKING_FUNCTION {
        const size_t read = m_read.load(std::memory_order_relaxed);
        const size_t count = std::min(num_samples, m_write.load(std::memory_order_acquire) - read);
        if (count == 0) { return 0; }
        copy_out(channels, read, count);
        m_read.store(read + count, std::memory_order_release);
        return count;
    }

    /// Reader: drops up to @p num_samples of the oldest arrived samples. Returns the samples
    /// dropped.
    size_t skip(size_t num_samples) TANH_NONBLOCKING_FUNCTION {
        const size_t read = m_read.load(std::memory_order_relaxed);
        const size_t count = std::min(num_samples, m_write.load(std::memory_order_acquire) - read);
        m_read.store(read + count, std::memory_order_release);
        return count;
    }

    /// Reader: the newest @p num_samples (or fewer, if fewer have arrived) into @p channels,
    /// consuming everything up to them: what an analyser that only shows the latest window
    /// wants. Returns the samples read.
    size_t read_latest(std::span<T* const> channels, size_t num_samples) TANH_NONBLOCKING_FUNCTION {
        // One snapshot of the writer's position for the skip and the copy: samples arriving in
        // between would otherwise make the window older than the newest at that snapshot.
        const size_t write = m_write.load(std::memory_order_acquire);
        const size_t read_position = m_read.load(std::memory_order_relaxed);
        const size_t count = std::min(num_samples, write - read_position);
        const size_t start = write - count;
        copy_out(channels, start, count);
        m_read.store(write, std::memory_order_release);
        return count;
    }

    // ---- any thread ------------------------------------------------------------------------

    /// The samples write() could not fit since prepare().
    [[nodiscard]] size_t dropped() const TANH_NONBLOCKING_FUNCTION {
        return m_dropped.load(std::memory_order_relaxed);
    }

private:
    static constexpr size_t k_cache_line = k_cache_line_size;

    /// Reader: positions [@p start, @p start + @p count) into @p channels (null ones skipped).
    void copy_out(std::span<T* const> channels,
                  size_t start,
                  size_t count) const TANH_NONBLOCKING_FUNCTION {
        if (count == 0) { return; }
        const size_t offset = start & m_mask;
        const size_t first = std::min(count, m_capacity - offset);
        const size_t channels_read = std::min(channels.size(), m_num_channels);
        for (size_t ch = 0; ch < channels_read; ++ch) {
            if (channels[ch] == nullptr) { continue; }
            const T* ring = channel(ch);
            std::copy_n(ring + offset, first, channels[ch]);
            std::copy_n(ring, count - first, channels[ch] + first);
        }
    }

    [[nodiscard]] T* channel(size_t ch) TANH_NONBLOCKING_FUNCTION {
        return m_samples.data() + (ch * m_capacity);
    }
    [[nodiscard]] const T* channel(size_t ch) const TANH_NONBLOCKING_FUNCTION {
        return m_samples.data() + (ch * m_capacity);
    }

    // The writer's line: its position, its drop count and the settings both sides read on every
    // call (the reader loads m_write on every call too, so they cost it no extra line).
    alignas(k_cache_line) std::atomic<size_t> m_write{0};
    size_t m_num_channels = 0;
    size_t m_capacity = 0;
    size_t m_mask = 0;
    std::atomic<size_t> m_dropped{0};
    std::vector<T> m_samples;  // channel after channel, m_capacity each
    // The reader's line, apart: the writer's stores never evict it.
    alignas(k_cache_line) std::atomic<size_t> m_read{0};
};

}  // namespace thl::inline THL_ABI_NAMESPACE::core
