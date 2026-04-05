#include <tanh/dsp/utils/PitchShifter.h>

#include <cmath>
#include <cstddef>
#include <tanh/core/Numbers.h>

namespace thl::dsp::utils {

PitchShifter::PitchShifter() = default;

void PitchShifter::prepare(int window_size) {
    m_window_size = window_size;
    m_buf_size = window_size * 2;
    m_buf.resize(1, static_cast<size_t>(m_buf_size));
    reset();
}

void PitchShifter::set_pitch(float semitones, float cents) {
    m_base_rate = std::pow(2.0f, (semitones + cents / 100.0f) / 12.0f);
    update_phase_inc(m_base_rate);
}

void PitchShifter::set_cents_modulation(float cents_offset) {
    update_phase_inc(m_base_rate * (1.0f + cents_offset * 0.00057779f));
}

void PitchShifter::reset() {
    m_buf.clear();
    m_write_pos = 0;
    m_phase = 0.0f;
}

float PitchShifter::process(float x) {
    float* buf = m_buf.get_write_pointer(0);
    buf[m_write_pos] = x;

    const float phase_a = m_phase;
    const float phase_b = std::fmod(m_phase + 0.5f, 1.0f);
    const auto n = static_cast<float>(m_window_size);

    const float delay_a = m_pitch_up ? (1.0f - phase_a) * n : phase_a * n;
    const float delay_b = m_pitch_up ? (1.0f - phase_b) * n : phase_b * n;

    constexpr float k_two_pi = 2.0f * std::numbers::pi_v<float>;
    const float w_a = 0.5f * (1.0f - std::cos(k_two_pi * phase_a));
    const float w_b = 0.5f * (1.0f - std::cos(k_two_pi * phase_b));

    const float val_a = read_interp(buf, delay_a);
    const float val_b = read_interp(buf, delay_b);

    m_phase = std::fmod(m_phase + m_phase_inc, 1.0f);
    m_write_pos = (m_write_pos + 1) % m_buf_size;

    return w_a * val_a + w_b * val_b;
}

void PitchShifter::update_phase_inc(float rate) {
    const float drift = rate - 1.0f;
    m_phase_inc = std::abs(drift) / static_cast<float>(m_window_size);
    m_pitch_up = rate >= 1.0f;
}

float PitchShifter::read_interp(const float* buf, float delay) const {
    const float pos = static_cast<float>(m_write_pos) - delay;
    const int idx = static_cast<int>(std::floor(pos));
    const float frac = pos - static_cast<float>(idx);
    const int i0 = ((idx % m_buf_size) + m_buf_size) % m_buf_size;
    const int i1 = (((idx + 1) % m_buf_size) + m_buf_size) % m_buf_size;
    return buf[i0] + frac * (buf[i1] - buf[i0]);
}

}  // namespace thl::dsp::utils
