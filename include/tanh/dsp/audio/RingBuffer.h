#pragma once

#include <tanh/core/Exports.h>
#include <tanh/dsp/audio/AudioBuffer.h>

#include <cstddef>
#include <vector>

namespace thl::dsp::audio {

class TANH_API RingBuffer {
public:
    RingBuffer() = default;

    void initialise_with_positions(size_t num_channels, size_t num_samples);
    void clear_with_positions();

    void push_sample(size_t channel, float sample);
    float pop_sample(size_t channel);

    void push_block(size_t channel, const float* data, size_t count);
    void pop_block(size_t channel, float* data, size_t count);

    float get_future_sample(size_t channel, size_t offset) const;
    float get_past_sample(size_t channel, size_t offset) const;

    size_t get_available_samples(size_t channel) const;
    size_t get_available_past_samples(size_t channel) const;

    size_t get_num_channels() const { return m_buffer.get_num_channels(); }
    size_t get_num_samples() const { return m_buffer.get_num_frames(); }

private:
    Buffer<float> m_buffer;
    std::vector<size_t> m_read_pos;
    std::vector<size_t> m_write_pos;
    std::vector<bool> m_is_full;
    std::vector<size_t> m_num_valid;
};

}  // namespace thl::dsp::audio
