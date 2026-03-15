#pragma once

namespace thl::dsp::utils {

/**
 * Sinusoidal oscillator using the coupled-form (magic circle) algorithm.
 *
 *   s1[n+1] = s1[n] + w·s2[n]
 *   s2[n+1] = s2[n] - w·s1[n+1]
 *
 * where w = 2·sin(π·freq/sampleRate). Output is s1 (sine).
 * Initialised with s2 = amplitude so the output ramps up from zero.
 *
 * Call prepare() before use.
 */
class SineOscillator {
public:
    SineOscillator();

    void prepare(float frequency, float sample_rate, float amplitude = 1.0f);
    void set_frequency(float frequency, float sample_rate);
    void reset();

    float process();

private:
    float m_s1        = 0.0f;
    float m_s2        = 0.0f;
    float m_w         = 0.0f;
    float m_amplitude = 1.0f;
};

}  // namespace thl::dsp::utils
