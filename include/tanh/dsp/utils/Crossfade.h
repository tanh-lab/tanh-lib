#pragma once

#include <tanh/core/AbiNamespace.h>
#include <tanh/core/BufferView.h>
#include <tanh/core/Numbers.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <cassert>
#include <cmath>
#include <cstddef>
#include <utility>

namespace thl::inline THL_ABI_NAMESPACE::dsp::utils {

enum class CrossfadeShape { Linear, EqualPower };

/**
 * Gains for the outgoing and incoming signals at @p position in [0, 1].
 *
 * @return {from_gain, to_gain}. EqualPower keeps from_gain^2 + to_gain^2 == 1.
 */
inline std::pair<float, float> crossfade_gains(float position,
                                               CrossfadeShape shape) TANH_NONBLOCKING_FUNCTION {
    switch (shape) {
        case CrossfadeShape::Linear: return {1.0f - position, position};
        case CrossfadeShape::EqualPower: {
            const float angle = position * 0.5f * std::numbers::pi_v<float>;
            return {std::cos(angle), std::sin(angle)};
        }
    }
    return {1.0f - position, position};
}

/**
 * Blends @p from into @p to across the whole length of @p out.
 *
 * Sample i uses position i / (n - 1), so the first sample is fully @p from and the last fully
 * @p to; a single sample is fully @p to. All three views must have the same number of channels
 * and samples (asserted in debug builds). @p out may alias @p from or @p to sample for sample.
 */
inline void crossfade(core::ConstBufferView from,
                      core::ConstBufferView to,
                      core::BufferView out,
                      CrossfadeShape shape) TANH_NONBLOCKING_FUNCTION {
    const size_t num_channels = out.get_num_channels();
    const size_t num_samples = out.get_num_samples();
    assert(from.get_num_channels() == num_channels && to.get_num_channels() == num_channels &&
           "crossfade: views have different channel counts");
    assert(from.get_num_samples() == num_samples && to.get_num_samples() == num_samples &&
           "crossfade: views have different lengths");

    const float position_step = num_samples > 1 ? 1.0f / static_cast<float>(num_samples - 1) : 0.0f;
    const float first_position = num_samples > 1 ? 0.0f : 1.0f;

    for (size_t channel = 0; channel < num_channels; ++channel) {
        const float* from_samples = from.get_read_pointer(channel);
        const float* to_samples = to.get_read_pointer(channel);
        float* out_samples = out.get_write_pointer(channel);
        for (size_t i = 0; i < num_samples; ++i) {
            const float position = first_position + static_cast<float>(i) * position_step;
            const auto [from_gain, to_gain] = crossfade_gains(position, shape);
            out_samples[i] = from_gain * from_samples[i] + to_gain * to_samples[i];
        }
    }
}

}  // namespace thl::inline THL_ABI_NAMESPACE::dsp::utils
