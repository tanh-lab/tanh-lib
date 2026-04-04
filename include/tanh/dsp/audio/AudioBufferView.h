#pragma once

#include <cstddef>
#include <span>
#include <type_traits>

namespace thl::dsp::audio {

template <typename T>
class Buffer;

template <typename T>
class BasicAudioBufferView {
    static_assert(std::is_same_v<T, float> || std::is_same_v<T, const float>,
                  "T must be float or const float");

public:
    BasicAudioBufferView() = default;

    BasicAudioBufferView(T* const* channels, size_t num_channels, size_t num_frames)
        : m_channels(channels), m_num_channels(num_channels), m_num_frames(num_frames) {}

    BasicAudioBufferView(T* mono, size_t num_frames)
        : m_inline_channel(mono)
        , m_channels(&m_inline_channel)
        , m_num_channels(1)
        , m_num_frames(num_frames) {}

    BasicAudioBufferView(Buffer<std::remove_const_t<T>>& buffer)
        : m_channels(buffer.get_array_of_write_pointers())
        , m_num_channels(buffer.get_num_channels())
        , m_num_frames(buffer.get_num_frames()) {}

    template <typename U = T, typename = std::enable_if_t<std::is_const_v<U>>>
    BasicAudioBufferView(const Buffer<std::remove_const_t<T>>& buffer)
        : m_channels(reinterpret_cast<const float* const*>(buffer.get_array_of_read_pointers()))
        , m_num_channels(buffer.get_num_channels())
        , m_num_frames(buffer.get_num_frames()) {}

    // Implicit conversion: mutable view -> const view
    template <typename U = T, typename = std::enable_if_t<std::is_const_v<U>>>
    BasicAudioBufferView(const BasicAudioBufferView<float>& other)
        : m_frame_offset(other.get_frame_offset())
        , m_num_channels(other.get_num_channels())
        , m_num_frames(other.get_num_frames()) {
        if (other.is_mono_inline()) {
            m_inline_channel = other.get_read_pointer(0);
            m_channels = &m_inline_channel;
            m_frame_offset = 0;
        } else {
            m_channels = reinterpret_cast<const float* const*>(other.get_raw_channels());
        }
    }

    // Copy constructor with mono fixup
    BasicAudioBufferView(const BasicAudioBufferView& other)
        : m_inline_channel(other.m_inline_channel)
        , m_channels(other.m_channels)
        , m_frame_offset(other.m_frame_offset)
        , m_num_channels(other.m_num_channels)
        , m_num_frames(other.m_num_frames) {
        if (other.is_mono_inline()) { m_channels = &m_inline_channel; }
    }

    // Move constructor with mono fixup
    BasicAudioBufferView(BasicAudioBufferView&& other) noexcept
        : m_inline_channel(other.m_inline_channel)
        , m_channels(other.m_channels)
        , m_frame_offset(other.m_frame_offset)
        , m_num_channels(other.m_num_channels)
        , m_num_frames(other.m_num_frames) {
        if (other.is_mono_inline()) { m_channels = &m_inline_channel; }
    }

    // Copy assignment with mono fixup
    BasicAudioBufferView& operator=(const BasicAudioBufferView& other) {
        if (this != &other) {
            m_inline_channel = other.m_inline_channel;
            m_channels = other.m_channels;
            m_frame_offset = other.m_frame_offset;
            m_num_channels = other.m_num_channels;
            m_num_frames = other.m_num_frames;
            if (other.is_mono_inline()) { m_channels = &m_inline_channel; }
        }
        return *this;
    }

    // Move assignment with mono fixup
    BasicAudioBufferView& operator=(BasicAudioBufferView&& other) noexcept {
        if (this != &other) {
            m_inline_channel = other.m_inline_channel;
            m_channels = other.m_channels;
            m_frame_offset = other.m_frame_offset;
            m_num_channels = other.m_num_channels;
            m_num_frames = other.m_num_frames;
            if (other.is_mono_inline()) { m_channels = &m_inline_channel; }
        }
        return *this;
    }

    const T* get_read_pointer(size_t channel) const { return m_channels[channel] + m_frame_offset; }

    template <typename U = T, typename = std::enable_if_t<!std::is_const_v<U>>>
    T* get_write_pointer(size_t channel) {
        return m_channels[channel] + m_frame_offset;
    }

    size_t get_num_channels() const { return m_num_channels; }
    size_t get_num_frames() const { return m_num_frames; }

    std::span<T> operator[](size_t channel) {
        return std::span<T>(m_channels[channel] + m_frame_offset, m_num_frames);
    }

    std::span<const T> operator[](size_t channel) const {
        return std::span<const T>(m_channels[channel] + m_frame_offset, m_num_frames);
    }

    T* const* get_raw_channels() const { return m_channels; }
    size_t get_frame_offset() const { return m_frame_offset; }

    bool is_mono_inline() const { return m_channels == &m_inline_channel; }

    BasicAudioBufferView sub_block(size_t start_frame, size_t num_frames) const {
        if (is_mono_inline()) {
            return BasicAudioBufferView(m_inline_channel + m_frame_offset + start_frame,
                                        num_frames);
        }
        BasicAudioBufferView result;
        result.m_channels = m_channels;
        result.m_frame_offset = m_frame_offset + start_frame;
        result.m_num_channels = m_num_channels;
        result.m_num_frames = num_frames;
        return result;
    }

private:
    T* m_inline_channel = nullptr;
    T* const* m_channels = nullptr;
    size_t m_frame_offset = 0;
    size_t m_num_channels = 0;
    size_t m_num_frames = 0;
};

using AudioBufferView = BasicAudioBufferView<float>;
using ConstAudioBufferView = BasicAudioBufferView<const float>;

}  // namespace thl::dsp::audio
