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
                                                               // doesn't work with std::ranges::fill
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

    void push_block(size_t channel, const T* data, size_t count) {
        const size_t capacity = m_buffer.get_num_samples();
        if (capacity == 0) { return; }
        T* buf = m_buffer.get_write_pointer(channel);
        for (size_t i = 0; i < count; ++i) {
            buf[m_write_pos[channel]] = data[i];
            m_write_pos[channel] = (m_write_pos[channel] + 1) % capacity;
            if (m_is_full[channel]) {
                m_read_pos[channel] = m_write_pos[channel];
            } else {
                if (m_num_valid[channel] < capacity) { ++m_num_valid[channel]; }
            }
            m_is_full[channel] = (m_write_pos[channel] == m_read_pos[channel]);
        }
    }

    void pop_block(size_t channel, T* data, size_t count) {
        const size_t capacity = m_buffer.get_num_samples();
        const T* buf = m_buffer.get_read_pointer(channel);
        for (size_t i = 0; i < count; ++i) {
            if (empty(channel)) {
                data[i] = T{};
                continue;
            }
            data[i] = buf[m_read_pos[channel]];
            m_read_pos[channel] = (m_read_pos[channel] + 1) % capacity;
            m_is_full[channel] = false;
        }
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
        if (m_is_full[channel]) { return capacity; }
        return (m_write_pos[channel] + capacity - m_read_pos[channel]) % capacity;
    }

    size_t get_available_past_samples(size_t channel) const {
        return m_num_valid[channel] - get_available_samples(channel);
    }

    size_t get_num_channels() const { return m_buffer.get_num_channels(); }
    size_t get_num_samples() const { return m_buffer.get_num_samples(); }

private:
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
