#include "tanh/modulation/LFOSource.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <tanh/core/Numbers.h>

using namespace thl::modulation;

void LFOSourceImpl::prepare(double sample_rate, size_t samples_per_block, uint32_t voice_count) {
    m_sample_rate = sample_rate;
    m_phase_increment = static_cast<float>(get_parameter<float>(Frequency) / sample_rate);
    resize_buffers(samples_per_block, voice_count);
    m_samples_until_update = 0;

    // Fully ramped in by default — a freshly-prepared LFO is "live" unless the
    // owner explicitly resets fade-in (e.g. on activation transition).
    m_fade_in_value = 1.0f;
    m_held_initialized = false;
    m_smoothed_value = 0.0f;

    m_phase_atomic.store(m_phase, std::memory_order_relaxed);
    m_fade_in_atomic.store(m_fade_in_value, std::memory_order_relaxed);
}

void LFOSourceImpl::reset_fade_in() {
    m_fade_in_value = 0.0f;
}

float LFOSourceImpl::next_random() {
    // xorshift32
    uint32_t x = m_prng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    m_prng_state = x;
    // Map to [-1, +1].
    return (static_cast<float>(x) / static_cast<float>(UINT32_MAX)) * 2.0f - 1.0f;
}

float LFOSourceImpl::generate_sample_basic(float phase, LFOWaveform waveform, float pulse_width) {
    constexpr float k_two_pi = 2.0f * std::numbers::pi_v<float>;

    switch (waveform) {
        case LFOWaveform::Sine: return std::sin(phase * k_two_pi);

        case LFOWaveform::Triangle:
            if (phase < 0.25f) { return phase * 4.0f; }
            if (phase < 0.75f) { return 2.0f - phase * 4.0f; }
            return phase * 4.0f - 4.0f;

        case LFOWaveform::Saw: return 2.0f * phase - 1.0f;

        case LFOWaveform::SawDown: return 1.0f - 2.0f * phase;

        case LFOWaveform::Square: {
            const float pw = std::clamp(pulse_width, 0.0f, 1.0f);
            return phase < pw ? 1.0f : -1.0f;
        }

        // SampleAndHold is handled in process() via m_held_value, not here.
        default: return 0.0f;
    }
}

void LFOSourceImpl::process(size_t num_samples, size_t offset) {
    for (size_t i = offset; i < offset + num_samples; ++i) {
        const auto sample_idx = static_cast<uint32_t>(i);

        const float freq = get_parameter<float>(Frequency, sample_idx);
        m_phase_increment = static_cast<float>(freq / m_sample_rate);

        if (m_samples_until_update == 0) {
            const auto decimation =
                static_cast<uint32_t>(get_parameter<int>(Decimation, sample_idx));
            const auto waveform =
                static_cast<LFOWaveform>(get_parameter<int>(Waveform, sample_idx));
            const float phase_offset = get_parameter<float>(PhaseOffset, sample_idx);
            const float bias = get_parameter<float>(Bias, sample_idx);
            const float pulse_width = get_parameter<float>(PulseWidth, sample_idx);
            const float depth = get_parameter<float>(Depth, sample_idx);
            const auto polarity =
                static_cast<LFOPolarity>(get_parameter<int>(Polarity, sample_idx));
            const float smooth = get_parameter<float>(Smooth, sample_idx);
            const float fade_in_s = get_parameter<float>(FadeIn, sample_idx);

            // Effective phase with offset, wrapped to [0, 1).
            float eff_phase = m_phase + phase_offset;
            eff_phase -= std::floor(eff_phase);

            // Generate the raw waveform sample. S&H latches a fresh random
            // value whenever the (unoffset) phase crosses zero; the held
            // value persists between crossings.
            float raw = 0.0f;
            if (waveform == LFOWaveform::SampleAndHold) {
                if (!m_held_initialized) {
                    m_held_value = next_random();
                    m_held_initialized = true;
                }
                raw = m_held_value;
            } else {
                raw = generate_sample_basic(eff_phase, waveform, pulse_width);
            }

            // Smoothing slew: one-pole low-pass with tau in [0, ~1s].
            const float tau_samples = std::max(1.0f, smooth * static_cast<float>(m_sample_rate));
            const float alpha = 1.0f - std::exp(-1.0f / tau_samples);
            if (smooth <= 0.0f) {
                m_smoothed_value = raw;
            } else {
                m_smoothed_value += alpha * (raw - m_smoothed_value);
            }

            // Bias + polarity mapping.
            float shaped = m_smoothed_value + bias;
            if (polarity == LFOPolarity::Unipolar) { shaped = shaped * 0.5f + 0.5f; }

            // Depth scale + fade-in envelope (linear ramp 0 -> 1 over
            // fade_in_s seconds; instant when fade_in_s <= 0).
            if (m_fade_in_value < 1.0f) {
                if (fade_in_s <= 0.0f) {
                    m_fade_in_value = 1.0f;
                } else {
                    const float step = 1.0f / (fade_in_s * static_cast<float>(m_sample_rate));
                    m_fade_in_value = std::min(1.0f, m_fade_in_value + step);
                }
            }

            m_last_output = shaped * depth * m_fade_in_value;
            m_samples_until_update = decimation == 0 ? 1 : decimation;
            record_change_point(sample_idx);
        }
        --m_samples_until_update;

        m_output_buffer[i] = m_last_output;

        // Advance phase. On wrap, latch a new S&H value so subsequent
        // decimation ticks pick up the new held value.
        const float prev_phase = m_phase;
        m_phase += m_phase_increment;
        if (m_phase >= 1.0f) {
            m_phase -= 1.0f;
            m_held_value = next_random();
            m_held_initialized = true;
        } else if (m_phase < prev_phase) {
            // Defensive: handle negative-frequency wrap.
            m_held_value = next_random();
            m_held_initialized = true;
        }
    }

    m_phase_atomic.store(m_phase, std::memory_order_relaxed);
    m_fade_in_atomic.store(m_fade_in_value, std::memory_order_relaxed);
}
