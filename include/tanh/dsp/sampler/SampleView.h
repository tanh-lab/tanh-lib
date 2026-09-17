#pragma once

#include <tanh/core/Buffer.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace thl::dsp::sampler {

/// Signed frame position or offset in a sample. 64-bit everywhere (`long` is
/// 32-bit on Windows); unsigned lengths are `size_t`.
using FramePos = std::int64_t;

/// Most channels a sampler component reads or writes.
inline constexpr size_t k_max_channels = 16;

/**
 * @brief A non-owning, planar, read-only view of one sample.
 *
 * The sampler components never own audio: the host keeps the buffer alive
 * for as long as a view of it is in use (typically: while the audio thread
 * renders the block the view was handed to). A default-constructed view is
 * empty and reads as silence.
 *
 * @par Real-Time Safety
 * Copying and reading a view is real-time safe.
 */
struct SampleView {
    std::array<const float*, k_max_channels> m_channels{};
    size_t m_num_frames{0};
    size_t m_num_channels{0};

    /// True when the view has audio to read.
    [[nodiscard]] bool valid() const { return m_num_frames > 0 && m_num_channels > 0; }

    /// View of a buffer (up to k_max_channels channels). An empty buffer
    /// gives an empty view.
    static SampleView of(const core::BufferF& buffer) {
        SampleView view;
        if (buffer.empty()) { return view; }
        view.m_num_frames = buffer.get_num_samples();
        view.m_num_channels = std::min(buffer.get_num_channels(), k_max_channels);
        for (size_t ch = 0; ch < view.m_num_channels; ++ch) {
            view.m_channels[ch] = buffer.get_read_pointer(ch);
        }
        return view;
    }

    /// View of planar pointers the caller owns.
    static SampleView of(const float* const* channels, size_t num_channels, size_t num_frames) {
        SampleView view;
        if (channels == nullptr || num_frames == 0) { return view; }
        view.m_num_frames = num_frames;
        view.m_num_channels = std::min(num_channels, k_max_channels);
        for (size_t ch = 0; ch < view.m_num_channels; ++ch) { view.m_channels[ch] = channels[ch]; }
        return view;
    }
};

/**
 * @brief Linear interpolation at `position`, wrapping past either end.
 *
 * For grains that may be scheduled to overshoot the sample, or to run
 * backwards past frame 0. Single precision: the grain read.
 */
inline float interpolate_wrapped(const float* data, size_t frames, float position) {
    auto pos_floor = static_cast<FramePos>(std::floor(position));
    float const frac = position - static_cast<float>(pos_floor);
    auto const wrap = static_cast<FramePos>(frames);
    while (pos_floor >= wrap) { pos_floor -= wrap; }
    while (pos_floor < 0) { pos_floor += wrap; }  // a reversed grain runs down past 0
    FramePos pos_ceil = pos_floor + 1;
    if (pos_ceil >= wrap) { pos_ceil -= wrap; }
    return data[pos_floor] * (1.0f - frac) + data[pos_ceil] * frac;
}

/// Wrapped read of `channel` (silence for a missing channel or empty view).
inline float read_wrapped(const SampleView& view, size_t channel, float position) {
    if (!view.valid() || channel >= view.m_num_channels) { return 0.0f; }
    return interpolate_wrapped(view.m_channels[channel], view.m_num_frames, position);
}

/**
 * @brief Linear interpolation at `position`, clamped to the first / last
 * frame.
 *
 * Double precision (a float position loses sample-exact addressing past
 * ~2^24 frames). A head crossfading out past the end of a sample holds its
 * last frame instead of folding back onto the audio the incoming head plays.
 */
inline float read_clamped(const SampleView& view, size_t channel, double position) {
    if (!view.valid() || channel >= view.m_num_channels) { return 0.0f; }
    size_t const frames = view.m_num_frames;
    double const pos = std::clamp(position, 0.0, static_cast<double>(frames - 1));
    auto const frame_a = static_cast<size_t>(pos);
    auto const frame_b = std::min(frame_a + 1, frames - 1);
    auto const frac = static_cast<float>(pos - static_cast<double>(frame_a));
    const float* data = view.m_channels[channel];
    return data[frame_a] * (1.0f - frac) + data[frame_b] * frac;
}

}  // namespace thl::dsp::sampler
