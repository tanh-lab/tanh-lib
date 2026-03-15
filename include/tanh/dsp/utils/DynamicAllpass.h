#pragma once

#include <tanh/dsp/utils/DynamicDelayLine.h>

namespace thl::dsp::utils {

/**
 * Allpass filter with heap-allocated (AudioBuffer-backed) delay line.
 *
 * Mirrors Allpass<float, N> but with runtime buffer size.
 * Lattice form: w[n] = x[n] + c·d[n-N],  y[n] = -c·w[n] + d[n-N]
 *
 * Accepts a float delay so the length can be modulated per sample.
 * Also exposes tap() to read internal delay-line values for reverb
 * output tap matrices.
 */
class DynamicAllpass {
public:
    DynamicAllpass();
    ~DynamicAllpass();

    void prepare(size_t max_delay);
    void reset();

    float process(float sample, float delay, float coefficient);

    // Read from the internal delay line relative to last write.
    float tap(float offset) const;
    float tap(size_t offset) const;

    DynamicDelayLine& line();
    const DynamicDelayLine& line() const;

private:
    DynamicDelayLine m_line;

    DynamicAllpass(const DynamicAllpass&) = delete;
    DynamicAllpass& operator=(const DynamicAllpass&) = delete;
};

}  // namespace thl::dsp::utils
