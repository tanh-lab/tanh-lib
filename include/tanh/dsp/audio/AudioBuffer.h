#pragma once

#include <tanh/dsp/audio/MemoryBlock.h>

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

namespace thl::dsp::audio {

/**
 * Planar audio buffer backed by a contiguous MemoryBlock with cached
 * channel pointers.
 *
 * Memory layout:
 *   ch0[0..N-1], ch1[0..N-1], ...
 *
 * Supports zero-copy swap_data() operations and direct channel-pointer
 * array access for interoperability with C-style audio APIs.
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
        if (m_num_channels > 0 && m_size > 0) {
            malloc_channels();
        }
    }

    Buffer(Buffer&& other) noexcept
        : m_num_channels(other.m_num_channels)
        , m_size(other.m_size)
        , m_sample_rate(other.m_sample_rate)
        , m_data(std::move(other.m_data))
        , m_channels(other.m_channels) {
        other.m_num_channels = 0;
        other.m_size = 0;
        other.m_sample_rate = 0.0;
        other.m_channels = nullptr;
    }

    ~Buffer() { std::free(m_channels); }

    Buffer& operator=(const Buffer& other) {
        if (this != &other) {
            std::free(m_channels);
            m_channels = nullptr;
            m_num_channels = other.m_num_channels;
            m_size = other.m_size;
            m_sample_rate = other.m_sample_rate;
            m_data = other.m_data;
            if (m_num_channels > 0 && m_size > 0) {
                malloc_channels();
            }
        }
        return *this;
    }

    Buffer& operator=(Buffer&& other) noexcept {
        if (this != &other) {
            std::free(m_channels);
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

    size_t get_num_frames() const { return m_size; }
    size_t get_num_channels() const { return m_num_channels; }
    double get_sample_rate() const { return m_sample_rate; }
    bool empty() const { return m_size == 0 || m_num_channels == 0; }

    // -- Channel pointer access -------------------------------------------

    const T* get_read_pointer(size_t channel) const {
        return m_channels[channel];
    }

    const T* get_read_pointer(size_t channel, size_t sample_index) const {
        return m_channels[channel] + sample_index;
    }

    T* get_write_pointer(size_t channel) {
        return m_channels[channel];
    }

    T* get_write_pointer(size_t channel, size_t sample_index) {
        return m_channels[channel] + sample_index;
    }

    const T* const* get_array_of_read_pointers() const {
        return const_cast<const T**>(m_channels);
    }

    T* const* get_array_of_write_pointers() { return m_channels; }

    // -- Raw data access --------------------------------------------------

    T* data() { return m_data.data(); }
    const T* data() const { return m_data.data(); }

    MemoryBlock<T>& get_memory_block() { return m_data; }

    // -- Sample access ----------------------------------------------------

    T get_sample(size_t channel, size_t sample_index) const {
        return m_channels[channel][sample_index];
    }

    void set_sample(size_t channel, size_t sample_index, T value) {
        m_channels[channel][sample_index] = value;
    }

    // -- Resize / clear ---------------------------------------------------

    void set_size(size_t num_channels, size_t num_frames,
                  double sample_rate) {
        m_num_channels = num_channels;
        m_size = num_frames;
        m_sample_rate = sample_rate;
        m_data.resize(num_channels * num_frames);
        std::free(m_channels);
        m_channels = nullptr;
        malloc_channels();
    }

    void resize(size_t num_channels, size_t num_frames) {
        m_num_channels = num_channels;
        m_size = num_frames;
        m_data.resize(num_channels * num_frames);
        std::free(m_channels);
        m_channels = nullptr;
        malloc_channels();
    }

    void clear() { m_data.clear(); }

    // -- Zero-copy swap ---------------------------------------------------

    void swap_data(Buffer& other) {
        if (this != &other) {
            if (m_num_channels == other.m_num_channels &&
                m_size == other.m_size) {
                m_data.swap_data(other.m_data);
                T** temp = m_channels;
                m_channels = other.m_channels;
                other.m_channels = temp;
            } else {
                std::cerr << "Buffer: cannot swap data, buffers have "
                             "different dimensions"
                          << std::endl;
            }
        }
    }

    void swap_data(MemoryBlock<T>& other) {
        if (other.size() == m_num_channels * m_size) {
            m_data.swap_data(other);
            reset_channel_ptr();
        } else {
            std::cerr << "Buffer: cannot swap data, MemoryBlock has "
                         "different size"
                      << std::endl;
        }
    }

    void swap_data(T*& raw_data, size_t size) {
        if (size == m_num_channels * m_size) {
            m_data.swap_data(raw_data, size);
            reset_channel_ptr();
        } else {
            std::cerr << "Buffer: cannot swap data, size mismatch"
                      << std::endl;
        }
    }

    void reset_channel_ptr() {
        for (size_t i = 0; i < m_num_channels; ++i) {
            m_channels[i] = m_data.data() + i * m_size;
        }
    }

private:
    void malloc_channels() {
        if (m_num_channels == 0) return;
        void* channels = std::malloc(m_num_channels * sizeof(T*));
        if (channels != nullptr) {
            m_channels = static_cast<T**>(channels);
        } else {
            std::cerr << "Buffer: failed to allocate channel pointers"
                      << std::endl;
            return;
        }
        for (size_t i = 0; i < m_num_channels; ++i) {
            m_channels[i] = m_data.data() + i * m_size;
        }
    }

    size_t m_num_channels = 0;
    size_t m_size = 0;
    double m_sample_rate = 0.0;
    T** m_channels = nullptr;
    MemoryBlock<T> m_data;
};

using AudioBuffer = Buffer<float>;

/// Copy planar buffer to an interleaved float vector.
inline std::vector<float> to_interleaved(const AudioBuffer& buffer) {
    size_t num_channels = buffer.get_num_channels();
    size_t num_frames = buffer.get_num_frames();
    std::vector<float> interleaved(num_frames * num_channels);
    for (size_t ch = 0; ch < num_channels; ++ch) {
        const float* src = buffer.get_read_pointer(ch);
        for (size_t f = 0; f < num_frames; ++f) {
            interleaved[f * num_channels + ch] = src[f];
        }
    }
    return interleaved;
}

/// Build a planar AudioBuffer from interleaved float data.
inline AudioBuffer from_interleaved(const float* data,
                                    size_t num_channels,
                                    size_t num_frames,
                                    double sample_rate) {
    AudioBuffer buffer(num_channels, num_frames, sample_rate);
    for (size_t ch = 0; ch < num_channels; ++ch) {
        float* dst = buffer.get_write_pointer(ch);
        for (size_t f = 0; f < num_frames; ++f) {
            dst[f] = data[f * num_channels + ch];
        }
    }
    return buffer;
}

}  // namespace thl::dsp::audio
