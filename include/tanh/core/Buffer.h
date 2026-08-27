// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <tanh/core/BufferView.h>
#include <tanh/core/MemoryBlock.h>

#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

namespace thl::core {

/**
 * Planar audio buffer backed by a contiguous MemoryBlock with cached
 * channel pointers.
 *
 * Memory layout:
 *   ch0[0..N-1], ch1[0..N-1], ...
 *
 * Supports zero-copy swap_data() operations and direct channel-pointer
 * array access for interoperability with C-style audio APIs.
 *
 * Failure semantics (no logging, no platform dependencies):
 *  - Allocation failure throws std::bad_alloc.
 *  - swap_data() with mismatched dimensions is a contract violation: it
 *    asserts in debug builds and is a no-op in release builds.
 */
template <typename T>
class Buffer {
public:
    Buffer() = default;

    Buffer(size_t num_channels, size_t num_frames, double sample_rate = 0.0)
        : m_num_channels(num_channels)
        , m_size(num_frames)
        , m_sample_rate(sample_rate)
        , m_data(num_channels * num_frames) {
        malloc_channels();
        clear();
    }

    Buffer(const Buffer& other)
        : m_num_channels(other.m_num_channels)
        , m_size(other.m_size)
        , m_sample_rate(other.m_sample_rate)
        , m_data(other.m_data) {
        if (m_num_channels > 0) { malloc_channels(); }
    }

    Buffer(Buffer&& other) noexcept
        : m_num_channels(other.m_num_channels)
        , m_size(other.m_size)
        , m_sample_rate(other.m_sample_rate)
        , m_channels(other.m_channels)
        , m_data(std::move(other.m_data)) {
        other.m_num_channels = 0;
        other.m_size = 0;
        other.m_sample_rate = 0.0;
        other.m_channels = nullptr;
    }

    ~Buffer() { std::free(static_cast<void*>(m_channels)); }

    /// Copy-and-swap: if the copy throws (std::bad_alloc) *this is unchanged.
    Buffer& operator=(const Buffer& other) {
        if (this != &other) {
            Buffer copy(other);
            *this = std::move(copy);
        }
        return *this;
    }

    Buffer& operator=(Buffer&& other) noexcept {
        if (this != &other) {
            std::free(static_cast<void*>(m_channels));
            m_num_channels = other.m_num_channels;
            m_size = other.m_size;
            m_sample_rate = other.m_sample_rate;
            m_data = std::move(other.m_data);
            m_channels = other.m_channels;
            other.m_num_channels = 0;
            other.m_size = 0;
            other.m_sample_rate = 0.0;
            other.m_channels = nullptr;
        }
        return *this;
    }

    // -- Dimensions and metadata ------------------------------------------

    size_t get_num_samples() const { return m_size; }
    size_t get_num_channels() const { return m_num_channels; }
    double get_sample_rate() const { return m_sample_rate; }
    bool empty() const { return m_size == 0 || m_num_channels == 0; }

    // -- Channel pointer access -------------------------------------------

    const T* get_read_pointer(size_t channel) const { return m_channels[channel]; }

    const T* get_read_pointer(size_t channel, size_t sample_index) const {
        return m_channels[channel] + sample_index;
    }

    // NOLINTNEXTLINE(clang-analyzer-core.NullDereference)
    T* get_write_pointer(size_t channel) { return m_channels[channel]; }

    T* get_write_pointer(size_t channel, size_t sample_index) {
        return m_channels[channel] + sample_index;
    }

    const T* const* get_array_of_read_pointers() const { return const_cast<const T**>(m_channels); }

    T* const* get_array_of_write_pointers() { return m_channels; }

    // -- Raw data access --------------------------------------------------

    T* data() { return m_data.data(); }
    const T* data() const { return m_data.data(); }

    MemoryBlock<T>& get_memory_block() { return m_data; }

    // -- Sample access ----------------------------------------------------

    T get_sample(size_t channel, size_t sample_index) const {
        if (m_channels == nullptr) { return T{}; }
        return m_channels[channel][sample_index];
    }

    void set_sample(size_t channel, size_t sample_index, T value) {
        if (m_channels == nullptr) { return; }
        m_channels[channel][sample_index] = value;
    }

    // -- Resize / clear ---------------------------------------------------

    /// Resize and set the sample rate. Contents are not preserved: the buffer
    /// is zeroed. If the allocation fails (std::bad_alloc) the buffer is left
    /// unchanged.
    void set_size(size_t num_channels, size_t num_frames, double sample_rate) {
        resize(num_channels, num_frames);
        m_sample_rate = sample_rate;
    }

    /// Resize; see set_size(). Leaves the buffer unchanged if allocation fails.
    void resize(size_t num_channels, size_t num_frames) {
        // Allocate everything that can fail before touching any member, so a
        // caught std::bad_alloc never leaves the dimensions ahead of the data.
        MemoryBlock<T> data(num_channels * num_frames);
        data.clear();
        T** channels = nullptr;
        if (num_channels > 0) {
            void* raw = std::malloc(num_channels * sizeof(T*));
            if (raw == nullptr) { throw std::bad_alloc(); }
            channels = static_cast<T**>(raw);
        }
        std::free(static_cast<void*>(m_channels));
        m_channels = channels;
        m_data = std::move(data);
        m_num_channels = num_channels;
        m_size = num_frames;
        reset_channel_ptr();
    }

    void clear() { m_data.clear(); }

    // -- Zero-copy swap ---------------------------------------------------

    /// Precondition: same channel count and frame count.
    void swap_data(Buffer& other) {
        if (this == &other) { return; }
        const bool same_dims = m_num_channels == other.m_num_channels && m_size == other.m_size;
        assert(same_dims && "Buffer: cannot swap data, buffers have different dimensions");
        if (!same_dims) { return; }
        m_data.swap_data(other.m_data);
        T** temp = m_channels;
        m_channels = other.m_channels;
        other.m_channels = temp;
    }

    /// Precondition: other.size() == get_num_channels() * get_num_samples().
    void swap_data(MemoryBlock<T>& other) {
        const bool same_size = other.size() == m_num_channels * m_size;
        assert(same_size && "Buffer: cannot swap data, MemoryBlock has different size");
        if (!same_size) { return; }
        m_data.swap_data(other);
        reset_channel_ptr();
    }

    /// Precondition: size == get_num_channels() * get_num_samples().
    void swap_data(T*& raw_data, size_t size) {
        const bool same_size = size == m_num_channels * m_size;
        assert(same_size && "Buffer: cannot swap data, size mismatch");
        if (!same_size) { return; }
        m_data.swap_data(raw_data, size);
        reset_channel_ptr();
    }

    void reset_channel_ptr() {
        for (size_t i = 0; i < m_num_channels; ++i) { m_channels[i] = m_data.data() + i * m_size; }
    }

    BasicBufferView<T> view() { return BasicBufferView<T>(*this); }
    BasicBufferView<const T> view() const { return BasicBufferView<const T>(*this); }

private:
    void malloc_channels() {
        if (m_num_channels == 0) { return; }
        void* channels = std::malloc(m_num_channels * sizeof(T*));
        if (channels == nullptr) { throw std::bad_alloc(); }
        m_channels = static_cast<T**>(channels);
        for (size_t i = 0; i < m_num_channels; ++i) { m_channels[i] = m_data.data() + i * m_size; }
    }

    size_t m_num_channels = 0;
    size_t m_size = 0;
    double m_sample_rate = 0.0;
    T** m_channels = nullptr;
    MemoryBlock<T> m_data;
};

using BufferF = Buffer<float>;

/// Copy planar buffer to an interleaved float vector.
inline std::vector<float> to_interleaved(const BufferF& buffer) {
    const size_t num_channels = buffer.get_num_channels();
    const size_t num_frames = buffer.get_num_samples();
    std::vector<float> interleaved(num_frames * num_channels);
    for (size_t ch = 0; ch < num_channels; ++ch) {
        const float* src = buffer.get_read_pointer(ch);
        for (size_t f = 0; f < num_frames; ++f) { interleaved[f * num_channels + ch] = src[f]; }
    }
    return interleaved;
}

/// Build a planar BufferF from interleaved float data.
inline BufferF from_interleaved(const float* data,
                                size_t num_channels,
                                size_t num_frames,
                                double sample_rate) {
    BufferF buffer(num_channels, num_frames, sample_rate);
    for (size_t ch = 0; ch < num_channels; ++ch) {
        float* dst = buffer.get_write_pointer(ch);
        for (size_t f = 0; f < num_frames; ++f) { dst[f] = data[f * num_channels + ch]; }
    }
    return buffer;
}

}  // namespace thl::core
