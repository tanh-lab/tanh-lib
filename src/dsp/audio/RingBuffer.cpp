#include <tanh/dsp/audio/RingBuffer.h>

#include <algorithm>
#include <cstring>

namespace thl::dsp::audio {

void RingBuffer::initialise_with_positions(size_t num_channels, size_t num_samples) {
    m_buffer.resize(num_channels, num_samples);
    m_buffer.clear();
    m_read_pos.assign(num_channels, 0);
    m_write_pos.assign(num_channels, 0);
    m_is_full.assign(num_channels, false);
    m_num_valid.assign(num_channels, 0);
}

void RingBuffer::clear_with_positions() {
    m_buffer.clear();
    std::fill(m_read_pos.begin(), m_read_pos.end(), 0);
    std::fill(m_write_pos.begin(), m_write_pos.end(), 0);
    std::fill(m_is_full.begin(), m_is_full.end(), false);
    std::fill(m_num_valid.begin(), m_num_valid.end(), 0);
}

void RingBuffer::push_sample(size_t channel, float sample) {
    const size_t capacity = m_buffer.get_num_frames();
    m_buffer.get_write_pointer(channel)[m_write_pos[channel]] = sample;
    m_write_pos[channel] = (m_write_pos[channel] + 1) % capacity;
    if (m_is_full[channel]) {
        m_read_pos[channel] = m_write_pos[channel];
    } else {
        if (m_num_valid[channel] < capacity) { ++m_num_valid[channel]; }
    }
    m_is_full[channel] = (m_write_pos[channel] == m_read_pos[channel]);
}

float RingBuffer::pop_sample(size_t channel) {
    const size_t capacity = m_buffer.get_num_frames();
    float sample = m_buffer.get_read_pointer(channel)[m_read_pos[channel]];
    m_read_pos[channel] = (m_read_pos[channel] + 1) % capacity;
    m_is_full[channel] = false;
    return sample;
}

void RingBuffer::push_block(size_t channel, const float* data, size_t count) {
    const size_t capacity = m_buffer.get_num_frames();
    float* buf = m_buffer.get_write_pointer(channel);
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

void RingBuffer::pop_block(size_t channel, float* data, size_t count) {
    const size_t capacity = m_buffer.get_num_frames();
    const float* buf = m_buffer.get_read_pointer(channel);
    for (size_t i = 0; i < count; ++i) {
        data[i] = buf[m_read_pos[channel]];
        m_read_pos[channel] = (m_read_pos[channel] + 1) % capacity;
        m_is_full[channel] = false;
    }
}

float RingBuffer::get_future_sample(size_t channel, size_t offset) const {
    const size_t capacity = m_buffer.get_num_frames();
    size_t pos = (m_read_pos[channel] + offset) % capacity;
    return m_buffer.get_read_pointer(channel)[pos];
}

float RingBuffer::get_past_sample(size_t channel, size_t offset) const {
    const size_t capacity = m_buffer.get_num_frames();
    size_t pos = (m_read_pos[channel] + capacity - offset) % capacity;
    return m_buffer.get_read_pointer(channel)[pos];
}

size_t RingBuffer::get_available_samples(size_t channel) const {
    const size_t capacity = m_buffer.get_num_frames();
    if (m_is_full[channel]) { return capacity; }
    return (m_write_pos[channel] + capacity - m_read_pos[channel]) % capacity;
}

size_t RingBuffer::get_available_past_samples(size_t channel) const {
    return m_num_valid[channel] - get_available_samples(channel);
}

}  // namespace thl::dsp::audio
