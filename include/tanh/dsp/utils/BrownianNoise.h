#pragma once

namespace thl::dsp::utils {

/**
 * Brownian motion (random walk) noise generator with one-pole smoothing.
 *
 * Integrates a velocity state that receives random impulses at `rate` Hz.
 * A spring-like restoring force and velocity damping keep the output
 * bounded in approximately [-1, 1] without hard clipping.
 *
 * Call prepare() before use.
 *
 * Parameters:
 *   rate     – impulse rate in Hz (controls how fast the value wanders)
 *   momentum – spring/damping strength [0, 1] (higher = stronger mean reversion)
 */
class BrownianNoise {
public:
    BrownianNoise();

    void prepare(float rate, float sample_rate, float momentum = 0.3f);
    void set_rate(float rate);
    void set_momentum(float momentum);
    void reset();

    float process();

private:
    static float calc_coeff(float rate, float sample_rate);

    float m_rate         = 2.0f;
    float m_sample_rate  = 48000.0f;
    float m_momentum     = 0.3f;
    float m_smooth_coeff = 0.0f;
    float m_value        = 0.0f;
    float m_velocity     = 0.0f;
    float m_smoothed     = 0.0f;
    float m_phase        = 0.0f;
};

}  // namespace thl::dsp::utils
