#pragma once

#include <array>

#include <tanh/core/Exports.h>

namespace thl::dsp::utils {

/**
 * Single-sideband frequency shifter using a Hilbert-transform approximation.
 *
 * Two parallel chains of four first-order allpass filters approximate a 90°
 * phase difference (Hilbert transform) over ~20 Hz – 20 kHz. The real and
 * imaginary outputs are multiplied by a complex exponential to shift all
 * frequencies by a constant Hz offset.
 *
 * Positive shiftHz shifts up; negative shifts down.
 * Allpass coefficients are pre-optimised for 44.1–48 kHz sample rates.
 *
 * Call prepare() before use.
 */
class TANH_API FrequencyShifter {
public:
    FrequencyShifter();

    void prepare(float shift_hz, float sample_rate);
    void set_shift(float hz);
    void reset();

    float process(float x);

private:
    struct State {
        float m_x1 = 0.0f;
        float m_y1 = 0.0f;
    };

    static float process_chain(float x, const float* coeffs, State* states);

    static const std::array<float, 4> k_coeffs_a;
    static const std::array<float, 4> k_coeffs_b;

    float m_sample_rate = 48000.0f;
    float m_phase = 0.0f;
    float m_phase_inc = 0.0f;
    std::array<State, 4> m_states_a;
    std::array<State, 4> m_states_b;
};

}  // namespace thl::dsp::utils
