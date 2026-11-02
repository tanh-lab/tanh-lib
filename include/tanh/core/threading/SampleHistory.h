// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <tanh/core/AbiNamespace.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <span>
#include <vector>

namespace thl::inline THL_ABI_NAMESPACE::core {

/**
 * The most recent samples of a multi-channel signal, written by one thread (the audio thread) and
 * read by any number of others (analysers, scopes, meters) without locks.
 *
 * The writer always writes: the oldest samples are overwritten, nothing is dropped, and the
 * writer neither waits nor knows its readers. A reader copies what it needs:
 * - read_latest(): the newest N samples, at most capacity(): an FFT, a scope, a goniometer, each
 *   with its own length and rate.
 * - read_since(): everything after the reader's own Cursor, for a reader that needs every sample
 *   (loudness, recording); if it fell more than capacity() behind, it is told how many it lost.
 * Every channel moves together: one write position for all.
 *
 * Overwrites during a read: a reader may be copying samples the writer is overwriting. The writer
 * claims the positions it is about to write (m_claim), writes them, then publishes them
 * (m_write); a reader copies, then checks the claim and keeps only what was not overwritten
 * meanwhile (Boehm's seqlock fences: "Can seqlocks get along with programming language memory
 * models?", 2012), retrying a few times before returning fewer samples. The samples are relaxed
 * atomics, so the overlap is well defined; on common targets they are plain loads and stores.
 *
 * Threads: prepare() allocates and must not run concurrently with anything else. write() belongs
 * to the one writer; written(), read_latest(), read_since() and cursor_now() may be called by
 * any number of readers. write() is wait-free, the reads lock-free; all are real-time safe.
 *
 * capacity() is twice the longest read given to prepare(), rounded up to a power of two.
 */
template <typename T>
class SampleHistory {
    static_assert(std::atomic<T>::is_always_lock_free,
                  "SampleHistory needs a T whose std::atomic is lock-free");

public:
    /// A reader's position for read_since(): the samples it has read up to.
    struct Cursor {
        size_t m_position = 0;
    };

    /// What read_since() did: samples copied, and samples lost (overwritten before they were read).
    struct ReadResult {
        size_t m_read = 0;
        size_t m_lost = 0;
    };

    static constexpr int k_max_retries = 4;

    SampleHistory() = default;
    SampleHistory(const SampleHistory&) = delete;
    SampleHistory& operator=(const SampleHistory&) = delete;

    /// Allocates @p num_channels channels for readers of up to @p max_read samples, all T{},
    /// nothing written. The history holds twice that (rounded up to a power of two): a reader
    /// copying its newest @p max_read samples has as many again before the writer reaches them,
    /// so a read practically never has to retry. Not concurrent with any other call.
    void prepare(size_t num_channels, size_t max_read) {
        m_num_channels = num_channels;
        m_capacity = std::bit_ceil(2 * std::max<size_t>(max_read, 1));
        m_mask = m_capacity - 1;
        m_samples = std::vector<std::atomic<T>>(m_num_channels * m_capacity);
        m_claim.store(0, std::memory_order_relaxed);
        m_write.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] size_t num_channels() const { return m_num_channels; }
    [[nodiscard]] size_t capacity() const { return m_capacity; }

    // ---- writer ----------------------------------------------------------------------------

    /// Writer: appends @p num_samples of every channel (@p channels[c]: channel c; a missing or
    /// null one is written as T{}, extra ones are ignored), overwriting the oldest. Of a block
    /// longer than capacity() only its last capacity() samples are kept.
    void write(std::span<const T* const> channels, size_t num_samples) TANH_NONBLOCKING_FUNCTION {
        const size_t position = m_write.load(std::memory_order_relaxed);
        const size_t end = position + num_samples;
        const size_t kept = std::min(num_samples, m_capacity);
        const size_t skipped = num_samples - kept;
        // Claim first: a reader that sees any of the new samples also sees the claim.
        m_claim.store(end, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        for (size_t ch = 0; ch < m_num_channels; ++ch) {
            std::atomic<T>* ring = channel(ch);
            const T* source =
                ch < channels.size() && channels[ch] != nullptr ? channels[ch] + skipped : nullptr;
            for (size_t i = 0; i < kept; ++i) {
                ring[(position + skipped + i) & m_mask].store(source != nullptr ? source[i] : T{},
                                                              std::memory_order_relaxed);
            }
        }
        m_write.store(end, std::memory_order_release);
    }

    // ---- readers ---------------------------------------------------------------------------

    /// The samples written since prepare() (it only grows): a position for read_since().
    [[nodiscard]] size_t written() const TANH_NONBLOCKING_FUNCTION {
        return m_write.load(std::memory_order_acquire);
    }

    /// A cursor at the newest sample: read_since() with it returns only what comes after.
    [[nodiscard]] Cursor cursor_now() const TANH_NONBLOCKING_FUNCTION { return {written()}; }

    /// Copies the newest @p num_samples (at most capacity() and what has been written) into
    /// @p channels (@p channels[c]: channel c; a null or missing destination skips that channel),
    /// oldest first. Returns the samples copied: fewer than asked only when not enough were
    /// written, or when the writer kept overwriting them through every retry.
    size_t read_latest(std::span<T* const> channels, size_t num_samples) TANH_NONBLOCKING_FUNCTION {
        size_t count = 0;
        for (int attempt = 0; attempt < k_max_retries; ++attempt) {  // see prepare(): rarely twice
            const size_t end = m_write.load(std::memory_order_acquire);
            count = std::min({num_samples, m_capacity, end});
            const size_t start = end - count;
            copy(channels, start, count, 0);
            const size_t lost = overwritten(start);
            if (lost == 0) { return count; }
            if (attempt + 1 == k_max_retries) {
                // Keep the part that is still valid, moved to the front.
                const size_t valid = count - std::min(lost, count);
                shift_to_front(channels, lost, valid);
                return valid;
            }
        }
        return 0;
    }

    /// Copies what was written after @p cursor, up to @p max_samples, into @p channels and moves
    /// the cursor past it. A cursor more than capacity() behind first jumps to the oldest sample
    /// still there; the samples it jumped over are reported as lost.
    ReadResult read_since(Cursor& cursor,
                          std::span<T* const> channels,
                          size_t max_samples) TANH_NONBLOCKING_FUNCTION {
        ReadResult result;
        for (int attempt = 0; attempt < k_max_retries; ++attempt) {
            const size_t end = m_write.load(std::memory_order_acquire);
            const size_t oldest = end > m_capacity ? end - m_capacity : 0;
            if (cursor.m_position < oldest) {
                result.m_lost += oldest - cursor.m_position;
                cursor.m_position = oldest;
            }
            const size_t start = cursor.m_position;
            const size_t count = std::min(max_samples, end - start);
            copy(channels, start, count, 0);
            const size_t lost = std::min(overwritten(start), count);
            if (lost == 0) {
                cursor.m_position = start + count;
                result.m_read = count;
                return result;
            }
            // The first `lost` were overwritten while copying: they are gone, try again after them.
            result.m_lost += lost;
            cursor.m_position = start + lost;
        }
        return result;
    }

private:
    static constexpr size_t k_cache_line = 64;

    [[nodiscard]] std::atomic<T>* channel(size_t ch) TANH_NONBLOCKING_FUNCTION {
        return m_samples.data() + (ch * m_capacity);
    }
    [[nodiscard]] const std::atomic<T>* channel(size_t ch) const TANH_NONBLOCKING_FUNCTION {
        return m_samples.data() + (ch * m_capacity);
    }

    /// Copies positions [@p start, @p start + @p count) to @p channels from index @p offset.
    void copy(std::span<T* const> channels,
              size_t start,
              size_t count,
              size_t offset) const TANH_NONBLOCKING_FUNCTION {
        const size_t channels_read = std::min(channels.size(), m_num_channels);
        for (size_t ch = 0; ch < channels_read; ++ch) {
            if (channels[ch] == nullptr) { continue; }
            const std::atomic<T>* ring = channel(ch);
            for (size_t i = 0; i < count; ++i) {
                channels[ch][offset + i] =
                    ring[(start + i) & m_mask].load(std::memory_order_relaxed);
            }
        }
    }

    /// After a copy from position @p start: how many of its first samples the writer may have
    /// overwritten meanwhile (0: the copy is valid).
    [[nodiscard]] size_t overwritten(size_t start) const TANH_NONBLOCKING_FUNCTION {
        std::atomic_thread_fence(std::memory_order_acquire);
        const size_t claim = m_claim.load(std::memory_order_relaxed);
        const size_t oldest_safe = claim > m_capacity ? claim - m_capacity : 0;
        return oldest_safe > start ? oldest_safe - start : 0;
    }

    void shift_to_front(std::span<T* const> channels,
                        size_t from,
                        size_t count) const TANH_NONBLOCKING_FUNCTION {
        const size_t channels_read = std::min(channels.size(), m_num_channels);
        for (size_t ch = 0; ch < channels_read; ++ch) {
            if (channels[ch] == nullptr) { continue; }
            std::copy_n(channels[ch] + from, count, channels[ch]);
        }
    }

    // The writer's line: its positions and the settings every call reads. Readers store nothing
    // shared, so nothing else needs a line of its own.
    alignas(k_cache_line) std::atomic<size_t> m_claim{0};  // positions being written up to
    std::atomic<size_t> m_write{0};                        // positions written up to
    size_t m_num_channels = 0;
    size_t m_capacity = 0;
    size_t m_mask = 0;
    std::vector<std::atomic<T>> m_samples;  // channel after channel, m_capacity each
};

}  // namespace thl::inline THL_ABI_NAMESPACE::core
