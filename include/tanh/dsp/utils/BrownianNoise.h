#pragma once

#include <tanh/core/Exports.h>

#include <cstddef>

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
class TANH_API BrownianNoise {
public:
    BrownianNoise();

    void prepare(float rate, float sample_rate, float momentum = 0.3f);
    void set_rate(float rate);
    void set_momentum(float momentum);
    void reset();

    float process();

    // Advance the generator by `num_samples` in O(1) and return the smoothed
    // value, for control-rate (once-per-block) use. Statistically equivalent
    // to `num_samples` process() calls for block sizes where at most one
    // impulse falls inside the block (rate * num_samples < sample_rate).
    float process_block(size_t num_samples);

private:
    static float calc_coeff(float rate, float sample_rate);

    float m_rate = 2.0f;
    float m_sample_rate = 48000.0f;
    float m_momentum = 0.3f;
    float m_smooth_coeff = 0.0f;
    float m_value = 0.0f;
    float m_velocity = 0.0f;
    float m_smoothed = 0.0f;
    float m_phase = 0.0f;
    size_t m_block_len = 0;      // cached num_samples for m_block_coeff
    float m_block_coeff = 0.0f;  // n-sample equivalent of m_smooth_coeff
};

}  // namespace thl::dsp::utils
