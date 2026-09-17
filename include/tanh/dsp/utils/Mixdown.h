#pragma once

#include <tanh/core/Buffer.h>

#include <cstddef>
#include <vector>

namespace thl::dsp::utils {

/**
 * @brief Mono average of every channel of `buffer`.
 *
 * Silence (all zeros, same length) when the buffer has no channels.
 *
 * @par Real-Time Safety
 * Allocates: for offline analysis (sample load, slicing, overviews).
 */
inline std::vector<float> mixdown(const core::BufferF& buffer) {
    size_t const num_frames = buffer.get_num_samples();
    size_t const num_channels = buffer.get_num_channels();
    std::vector<float> mono(num_frames, 0.0f);
    if (num_channels == 0) { return mono; }
    for (size_t ch = 0; ch < num_channels; ++ch) {
        const float* src = buffer.get_read_pointer(ch);
        for (size_t f = 0; f < num_frames; ++f) { mono[f] += src[f]; }
    }
    float const scale = 1.0f / static_cast<float>(num_channels);
    for (float& v : mono) { v *= scale; }
    return mono;
}

}  // namespace thl::dsp::utils
