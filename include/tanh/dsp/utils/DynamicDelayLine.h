#pragma once

#include <cstddef>
#include <cstdint>

#include <tanh/core/Exports.h>
#include <tanh/dsp/audio/AudioBuffer.h>

namespace thl::dsp::utils {

/**
 * Variable-length float delay line backed by an AudioBuffer (heap-allocated).
 *
 * Mirrors the API of DelayLine<float, N> but with runtime buffer sizing.
 * Also exposes tap() for reading relative to the most recent write, as
 * required by reverb output tap matrices.
 *
 * Call prepare() before first use to allocate the buffer.
 */
class TANH_API DynamicDelayLine {
public:
    DynamicDelayLine();
    ~DynamicDelayLine();

    void prepare(size_t max_delay);
    void reset();

    void set_delay(size_t delay);
    void write(float sample);
    float write_read(float sample, float delay);

    float read() const;
    float read(size_t delay) const;
    float read(float delay) const;
    float read_hermite(float delay) const;

    // Read `offset` samples ago relative to the most recent write.
    // tap(0) = last written sample; tap(N) = N samples before that.
    float tap(float offset) const;
    float tap(size_t offset) const;

private:
    float read_at(size_t delay) const;

    thl::dsp::audio::Buffer<float> m_buf;
    size_t m_max_delay = 1;
    size_t m_write_ptr = 0;
    size_t m_delay = 1;

    DynamicDelayLine(const DynamicDelayLine&) = delete;
    DynamicDelayLine& operator=(const DynamicDelayLine&) = delete;
};

}  // namespace thl::dsp::utils
