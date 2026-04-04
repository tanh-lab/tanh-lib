#pragma once

#include <tanh/dsp/audio/AudioBuffer.h>

namespace thl::dsp::utils {

/**
 * Two-grain overlap-add pitch shifter with Hann-window crossfade.
 *
 * Two read heads traverse a circular buffer at a rate proportional to the
 * pitch ratio, crossfaded with a Hann window so the two grains always sum
 * to 1.0, hiding grain boundaries.
 *
 * The circular buffer is backed by an AudioBuffer (heap-allocated).
 *
 * Call prepare() before use.
 */
class PitchShifter {
public:
    PitchShifter();

    void prepare(int window_size = 2048);
    void set_pitch(float semitones, float cents = 0.0f);

    // Apply a cents offset on top of the current base pitch.
    // Uses the fast linear approximation 2^(c/1200) ≈ 1 + c·ln2/1200.
    // Accurate within ~0.1% for |centsOffset| < 200.
    void set_cents_modulation(float cents_offset);
    void reset();

    float process(float x);

private:
    void update_phase_inc(float rate);
    float read_interp(const float* buf, float delay) const;

    thl::dsp::audio::Buffer<float> m_buf;
    int m_window_size = 2048;
    int m_buf_size = 4096;
    int m_write_pos = 0;
    float m_phase = 0.0f;
    float m_base_rate = 1.0f;
    float m_phase_inc = 0.0f;
    bool m_pitch_up = true;
};

}  // namespace thl::dsp::utils
