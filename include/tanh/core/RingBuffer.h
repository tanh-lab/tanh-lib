// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <tanh/core/Buffer.h>

#include <algorithm>
#include <cstddef>
#include <vector>

namespace thl::core {

/**
 * Multi-channel ring buffer over an arbitrary element type.
 *
 * Push semantics are delay-line friendly: pushing into a full channel
 * silently overwrites the oldest sample (the read position advances), so the
 * buffer can be driven continuously as a history/delay line. Popping an empty
 * channel returns a value-initialized element (T{}).
 *
 * The element type only needs to be copyable and value-initializable; float
 * for audio, integer types for token/index streams, etc.
 */
template <typename T>
class RingBuffer {
public:
    RingBuffer() = default;

    void initialise_with_positions(size_t num_channels, size_t num_samples) {
        m_buffer.resize(num_channels, num_samples);
        m_buffer.clear();
        m_read_pos.assign(num_channels, 0);
        m_write_pos.assign(num_channels, 0);
        m_is_full.assign(num_channels, false);
        m_num_valid.assign(num_channels, 0);
    }

    /// American-spelling alias (anira-compatible surface).
    void initialize_with_positions(size_t num_channels, size_t num_samples) {
        initialise_with_positions(num_channels, num_samples);
    }

    void clear_with_positions() {
        m_buffer.clear();
        std::ranges::fill(m_read_pos, 0);
        std::fill(m_is_full.begin(), m_is_full.end(), false);  // NOLINT(modernize-use-ranges)
                                                               // std::vector<bool> specialization
                                                               // doesn't work with
                                                               // std::ranges::fill
        std::ranges::fill(m_write_pos, 0);
        std::ranges::fill(m_num_valid, 0);
    }

    void push_sample(size_t channel, T sample) {
        const size_t capacity = m_buffer.get_num_samples();
        if (capacity == 0) { return; }
        m_buffer.get_write_pointer(channel)[m_write_pos[channel]] = sample;
        m_write_pos[channel] = (m_write_pos[channel] + 1) % capacity;
        if (m_is_full[channel]) {
            m_read_pos[channel] = m_write_pos[channel];
        } else {
            if (m_num_valid[channel] < capacity) { ++m_num_valid[channel]; }
        }
        m_is_full[channel] = (m_write_pos[channel] == m_read_pos[channel]);
    }

    T pop_sample(size_t channel) {
        if (empty(channel)) { return T{}; }
        const size_t capacity = m_buffer.get_num_samples();
        T const sample = m_buffer.get_read_pointer(channel)[m_read_pos[channel]];
        m_read_pos[channel] = (m_read_pos[channel] + 1) % capacity;
        m_is_full[channel] = false;
        return sample;
    }

    /// Push `count` samples. Equivalent to `count` calls of push_sample(), but
    /// copies in at most two contiguous chunks. If `count` exceeds the capacity
    /// only the last `capacity` samples are retained.
    void push_block(size_t channel, const T* data, size_t count) {
        const size_t capacity = m_buffer.get_num_samples();
        if (capacity == 0 || count == 0) { return; }
        T* buf = m_buffer.get_write_pointer(channel);
        const size_t free = capacity - get_available_samples(channel);

        // Only the tail of an oversized block can survive; skip the rest.
        size_t start = m_write_pos[channel];
        if (count > capacity) {
            data += count - capacity;
            start = (start + (count - capacity)) % capacity;
            count = capacity;
        }

        copy_wrapped(data, buf, start, count, capacity);
        m_write_pos[channel] = (start + count) % capacity;

        if (count >= free) {
            // Reached or overran the read position: the ring is now full and
            // the oldest retained sample sits right after the write head.
            m_read_pos[channel] = m_write_pos[channel];
            m_is_full[channel] = true;
        }
        m_num_valid[channel] = std::min(m_num_valid[channel] + count, capacity);
    }

    /// Pop `count` samples. Equivalent to `count` calls of pop_sample(): any
    /// samples beyond what is available are value-initialised (T{}).
    void pop_block(size_t channel, T* data, size_t count) {
        const size_t capacity = m_buffer.get_num_samples();
        const size_t available = get_available_samples(channel);
        const size_t n = std::min(count, available);

        if (n > 0) {
            const T* buf = m_buffer.get_read_pointer(channel);
            const size_t start = m_read_pos[channel];
            const size_t first = std::min(n, capacity - start);
            std::copy_n(buf + start, first, data);
            std::copy_n(buf, n - first, data + first);
            m_read_pos[channel] = (start + n) % capacity;
            m_is_full[channel] = false;
        }
        std::fill_n(data + n, count - n, T{});
    }

    T get_future_sample(size_t channel, size_t offset) const {
        const size_t capacity = m_buffer.get_num_samples();
        if (capacity == 0) { return T{}; }
        size_t const pos = (m_read_pos[channel] + offset) % capacity;
        return m_buffer.get_read_pointer(channel)[pos];
    }

    T get_past_sample(size_t channel, size_t offset) const {
        const size_t capacity = m_buffer.get_num_samples();
        if (capacity == 0) { return T{}; }
        size_t const pos = (m_read_pos[channel] + capacity - offset) % capacity;
        return m_buffer.get_read_pointer(channel)[pos];
    }

    size_t get_available_samples(size_t channel) const {
        const size_t capacity = m_buffer.get_num_samples();
        if (capacity == 0 || m_is_full[channel]) { return capacity; }
        return (m_write_pos[channel] + capacity - m_read_pos[channel]) % capacity;
    }

    size_t get_available_past_samples(size_t channel) const {
        return m_num_valid[channel] - get_available_samples(channel);
    }

    size_t get_num_channels() const { return m_buffer.get_num_channels(); }
    size_t get_num_samples() const { return m_buffer.get_num_samples(); }

private:
    /// Copy `count` elements from `src` into the ring starting at `start`,
    /// wrapping at `capacity`. Requires count <= capacity.
    static void copy_wrapped(const T* src, T* ring, size_t start, size_t count, size_t capacity) {
        const size_t first = std::min(count, capacity - start);
        std::copy_n(src, first, ring + start);
        std::copy_n(src + first, count - first, ring);
    }

    bool empty(size_t channel) const {
        return m_buffer.get_num_samples() == 0 ||
               (!m_is_full[channel] && m_read_pos[channel] == m_write_pos[channel]);
    }

    Buffer<T> m_buffer;
    std::vector<size_t> m_read_pos;
    std::vector<size_t> m_write_pos;
    std::vector<bool> m_is_full;
    std::vector<size_t> m_num_valid;
};

using RingBufferF = RingBuffer<float>;

}  // namespace thl::core
