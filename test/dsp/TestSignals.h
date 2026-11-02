#pragma once

// Test signals and measurements shared by the dsp tests.

#include <tanh/core/Buffer.h>
#include <tanh/core/BufferView.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>
#include <random>

namespace dsp_test {

using thl::core::Buffer;
using thl::core::BufferView;

/// 20 log10 |@p amplitude|.
inline double db(double amplitude) {
    return 20.0 * std::log10(std::abs(amplitude));
}

/// Every sample of every channel at @p value.
inline Buffer<float> constant(size_t num_channels, size_t num_samples, float value) {
    Buffer<float> signal(num_channels, num_samples);
    for (size_t ch = 0; ch < num_channels; ++ch) {
        std::fill_n(signal.get_write_pointer(ch), num_samples, value);
    }
    return signal;
}

/// Uniform noise in [-@p amplitude, @p amplitude], channel after channel from one generator.
inline Buffer<float> noise(size_t num_channels,
                           size_t num_samples,
                           float amplitude = 1.0f,
                           unsigned seed = 1) {
    std::mt19937 generator(seed);
    std::uniform_real_distribution<float> distribution(-amplitude, amplitude);
    Buffer<float> signal(num_channels, num_samples);
    for (size_t ch = 0; ch < num_channels; ++ch) {
        float* samples = signal.get_write_pointer(ch);
        for (size_t n = 0; n < num_samples; ++n) { samples[n] = distribution(generator); }
    }
    return signal;
}

/// The same sine of @p amplitude at @p frequency on every channel.
inline Buffer<float> sine(size_t num_channels,
                          size_t num_samples,
                          double frequency,
                          float amplitude,
                          double sample_rate,
                          double phase = 0.0) {
    Buffer<float> signal(num_channels, num_samples);
    for (size_t n = 0; n < num_samples; ++n) {
        const auto value = static_cast<float>(
            amplitude *
            std::sin((2.0 * std::numbers::pi * frequency * static_cast<double>(n) / sample_rate) +
                     phase));
        for (size_t ch = 0; ch < num_channels; ++ch) { signal.get_write_pointer(ch)[n] = value; }
    }
    return signal;
}

/// The RMS of channel @p ch over its second half (once filters and envelopes have settled).
inline double settled_rms(const Buffer<float>& signal, size_t ch = 0) {
    const size_t num_samples = signal.get_num_samples();
    const float* samples = signal.get_read_pointer(ch);
    double sum = 0.0;
    for (size_t n = num_samples / 2; n < num_samples; ++n) {
        sum += static_cast<double>(samples[n]) * static_cast<double>(samples[n]);
    }
    return std::sqrt(sum / static_cast<double>(num_samples - (num_samples / 2)));
}

/// The peak of channel @p ch over its second half.
inline float settled_peak(const Buffer<float>& signal, size_t ch = 0) {
    const size_t num_samples = signal.get_num_samples();
    const float* samples = signal.get_read_pointer(ch);
    float peak = 0.0f;
    for (size_t n = num_samples / 2; n < num_samples; ++n) {
        peak = std::max(peak, std::abs(samples[n]));
    }
    return peak;
}

/// @p processor's process() over @p signal in blocks of @p block_size, or process_with_dry()
/// into @p dry (same shape) when given.
template <typename Processor>
void process_in_blocks(Processor& processor,
                       Buffer<float>& signal,
                       size_t block_size,
                       Buffer<float>* dry = nullptr) {
    const size_t num_samples = signal.get_num_samples();
    for (size_t start = 0; start < num_samples; start += block_size) {
        const size_t length = std::min(block_size, num_samples - start);
        if (dry != nullptr) {
            processor.process_with_dry(BufferView(signal).sub_block(start, length),
                                       0,
                                       BufferView(*dry).sub_block(start, length));
        } else {
            processor.process(BufferView(signal).sub_block(start, length));
        }
    }
}

/// |H(@p frequency)| of a filter from @p length samples of its impulse response, so no settling
/// time or window is involved. @p tick(x) runs one sample through the filter.
template <typename Tick>
double impulse_magnitude(Tick&& tick, size_t length, double frequency, double sample_rate) {
    const double w = 2.0 * std::numbers::pi * frequency / sample_rate;
    std::complex<double> sum = 0.0;
    for (size_t n = 0; n < length; ++n) {
        const float h = tick(n == 0 ? 1.0f : 0.0f);
        sum += static_cast<double>(h) * std::polar(1.0, -w * static_cast<double>(n));
    }
    return std::abs(sum);
}

/// The bilinear transform's frequency ratio of @p frequency to @p cutoff: an analogue prototype
/// prewarped at the cutoff, evaluated at the ratio, is the digital filter's response.
inline double prewarped_ratio(double frequency, double cutoff, double sample_rate) {
    return std::tan(std::numbers::pi * frequency / sample_rate) /
           std::tan(std::numbers::pi * cutoff / sample_rate);
}

}  // namespace dsp_test
