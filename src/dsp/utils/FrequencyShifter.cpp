#include <tanh/dsp/utils/FrequencyShifter.h>

#include <array>
#include <cmath>
#include <numbers>

namespace thl::dsp::utils {

const std::array<float, 4> FrequencyShifter::k_coeffs_a = {std::numbers::ln2_v<float>,
                                                           // — Hilbert allpass
                                                           // coefficient, not ln2
                                                           0.9360654322959f,
                                                           0.9882295226860f,
                                                           0.9987488452737f};
const std::array<float, 4> FrequencyShifter::k_coeffs_b = {0.4021921162426f,
                                                           0.8561710882420f,
                                                           0.9722909545651f,
                                                           0.9952884791278f};

FrequencyShifter::FrequencyShifter() = default;

void FrequencyShifter::prepare(float shift_hz, float sample_rate) {
    m_sample_rate = sample_rate;
    set_shift(shift_hz);
    reset();
}

void FrequencyShifter::set_shift(float hz) {
    m_phase_inc = 2.0f * std::numbers::pi_v<float> * hz / m_sample_rate;
}

void FrequencyShifter::reset() {
    m_phase = 0.0f;
    for (auto& s : m_states_a) { s = {}; }
    for (auto& s : m_states_b) { s = {}; }
}

float FrequencyShifter::process(float x) {
    const float real = process_chain(x, k_coeffs_a.data(), m_states_a.data());
    const float imag = process_chain(x, k_coeffs_b.data(), m_states_b.data());

    const float out = real * std::cos(m_phase) - imag * std::sin(m_phase);

    m_phase += m_phase_inc;
    if (m_phase > std::numbers::pi_v<float>) {
        m_phase -= 2.0f * std::numbers::pi_v<float>;
    } else if (m_phase < -std::numbers::pi_v<float>) {
        m_phase += 2.0f * std::numbers::pi_v<float>;
    }

    return out;
}

float FrequencyShifter::process_chain(float x, const float* coeffs, State* states) {
    float out = x;
    for (int i = 0; i < 4; ++i) {
        const float y = coeffs[i] * (out - states[i].m_y1) + states[i].m_x1;
        states[i].m_x1 = out;
        states[i].m_y1 = y;
        out = y;
    }
    return out;
}

}  // namespace thl::dsp::utils
