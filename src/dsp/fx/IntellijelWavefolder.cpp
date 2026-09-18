#include <tanh/dsp/fx/IntellijelWavefolder.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>

#include "tanh/core/BufferView.h"

namespace thl::dsp::fx {

namespace {
// 30 ms linear ramp on every parameter. Each one sits inside the sine's phase,
// so a steeper ramp that is re-aimed every block kinks the phase at each block
// boundary and crackles (5 ms on Folds did). 30 ms is long enough that the
// harmonic sweep stays gentle, short enough that LFO modulation up to ~10 Hz
// still tracks usefully.
constexpr double k_param_smoothing_time = 0.030;

// One-pole DC blocker corner — 30 Hz removes the sustained DC the transfer
// function produces for non-zero Symmetry and also rejects most of the
// low-frequency drift during a Symmetry parameter ramp (a 120 ms ramp's
// spectral peak sits around 4 Hz, where a 30 Hz corner attenuates ~87%).
// 30 Hz is still well below the audible bass range post-wavefolder, where
// the transfer function generates its own harmonic content.
constexpr float k_dc_blocker_cutoff_hz = 30.0f;

// Release of the input peak follower. The attack is instant, so the folder's
// unit-domain input x / envelope never leaves [-1, 1] — a grain onset can't
// overshoot into a fold spike. 50 ms rides out the dips of a low sine without
// holding a decaying tail at its old level for long.
constexpr double k_envelope_release_time = 0.050;

// Envelope floor (-60 dBFS). Input below it is treated as silence and folded
// at the floor's scale: a silent input stays exactly silent and low-level noise
// is never normalised up into full-scale folding.
constexpr float k_envelope_floor = 1e-3f;

// Tone sweep: darkest lowpass cutoff at Tone 0, and the open end at Tone 1 as
// a fraction of the sample rate.
constexpr float k_tone_min_hz = 1000.0f;
constexpr float k_tone_max_ratio = 0.49f;

// Drive·(1 + Folds) at which the sine reaches its first peak for a full-scale
// unit input. The makeup gain is flat above it.
constexpr float k_first_fold = std::numbers::pi_v<float> * 0.5f;
}  // namespace

IntellijelWavefolderImpl::IntellijelWavefolderImpl() = default;
IntellijelWavefolderImpl::~IntellijelWavefolderImpl() = default;

void IntellijelWavefolderImpl::prepare(const double& sample_rate,
                                       const size_t& /*samples_per_block*/,
                                       const size_t& num_channels) {
    m_smoothed_drive.reset(sample_rate, k_param_smoothing_time);
    m_smoothed_folds.reset(sample_rate, k_param_smoothing_time);
    m_smoothed_symmetry.reset(sample_rate, k_param_smoothing_time);
    m_smoothed_tone.reset(sample_rate, k_param_smoothing_time);

    // Prime with current parameter values so prepare doesn't introduce a ramp
    // from zero on the first block.
    const float drive = std::clamp(get_parameter<float>(Drive), 0.1f, 20.0f);
    const float folds = std::clamp(get_parameter<float>(Folds), 0.0f, 10.0f);
    const float symmetry = std::clamp(get_parameter<float>(Symmetry), -1.0f, 1.0f);
    const float tone = std::clamp(get_parameter<float>(Tone), 0.0f, 1.0f);

    m_smoothed_drive.set_current_and_target_value(drive);
    m_smoothed_folds.set_current_and_target_value(folds);
    m_smoothed_symmetry.set_current_and_target_value(symmetry);
    m_smoothed_tone.set_current_and_target_value(tone);

    // The shaper subtracts its own zero-input value, so a silent input is zero
    // at the DC blocker from sample 0.
    m_dc_x_prev.assign(num_channels, 0.0f);
    m_dc_y_prev.assign(num_channels, 0.0f);
    m_dc_pole = std::exp(-2.0f * std::numbers::pi_v<float> * k_dc_blocker_cutoff_hz /
                         static_cast<float>(sample_rate));

    m_env_release = std::exp(
        -1.0f / static_cast<float>(k_envelope_release_time * static_cast<float>(sample_rate)));
    m_envelope.assign(num_channels, 0.0f);
    m_tone_state.assign(num_channels, {0.0f, 0.0f});
    m_sample_rate = static_cast<float>(sample_rate);
}

void IntellijelWavefolderImpl::process(thl::core::BufferView buffer, uint32_t modulation_offset) {
    m_smoothed_drive.set_target_value(
        std::clamp(get_parameter<float>(Drive, modulation_offset), 0.1f, 20.0f));
    m_smoothed_folds.set_target_value(
        std::clamp(get_parameter<float>(Folds, modulation_offset), 0.0f, 10.0f));
    m_smoothed_symmetry.set_target_value(
        std::clamp(get_parameter<float>(Symmetry, modulation_offset), -1.0f, 1.0f));
    m_smoothed_tone.set_target_value(
        std::clamp(get_parameter<float>(Tone, modulation_offset), 0.0f, 1.0f));

    const size_t num_channels = std::min(buffer.get_num_channels(), m_dc_x_prev.size());
    const size_t num_frames = buffer.get_num_samples();

    for (size_t i = 0; i < num_frames; ++i) {
        const float drive = m_smoothed_drive.get_smoothed_value(1);
        const float folds = m_smoothed_folds.get_smoothed_value(1);
        const float symmetry = m_smoothed_symmetry.get_smoothed_value(1);
        const float tone = m_smoothed_tone.get_smoothed_value(1);

        const float k = drive * (1.0f + folds);
        // Zero-input value of the shaper. Subtracting it keeps the Symmetry
        // offset from turning into DC that follows the input envelope (a thump
        // per grain the DC blocker can't fully remove).
        const float rest = shape(0.0f, k, symmetry);
        // Fixed makeup: the inverse of the symmetric shaper's peak for a
        // full-scale unit input. ~1/k while the sine is still linear (so low
        // Drive is unity gain), 1 once the first fold is reached. A function
        // of smoothed parameters only, so it can't step.
        const float makeup = 1.0f / std::sin(std::min(k, k_first_fold));
        const float tone_g = tone_coefficient(tone);

        for (size_t ch = 0; ch < num_channels; ++ch) {
            float* data = buffer.get_write_pointer(ch);
            const float x = data[i];

            // Peak follower: instant attack, exponential release.
            m_envelope[ch] = std::max(std::fabs(x), m_envelope[ch] * m_env_release);
            const float env = std::max(m_envelope[ch], k_envelope_floor);

            // Fold in the unit domain, where the input's own level no longer
            // decides how hard it is driven.
            const float u = x / env;
            const float s = (shape(u, k, symmetry) - rest) * makeup;

            const float blocked = s - m_dc_x_prev[ch] + m_dc_pole * m_dc_y_prev[ch];
            m_dc_x_prev[ch] = s;
            m_dc_y_prev[ch] = blocked;

            // Restore the input envelope, then darken by Tone: two cascaded
            // TPT one-pole lowpasses (12 dB/oct, no resonance, stable under
            // per-sample cutoff changes).
            float y = blocked * env;
            for (float& z : m_tone_state[ch]) {
                const float v = (y - z) * tone_g;
                y = v + z;
                z = y + v;
            }
            data[i] = y;
        }
    }
}

float IntellijelWavefolderImpl::shape(float u, float k, float symmetry) {
    return std::sin((u + symmetry) * k);
}

float IntellijelWavefolderImpl::tone_coefficient(float tone) const {
    // Exponential cutoff sweep; Tone 1 sits just below Nyquist, where the
    // lowpass is effectively open.
    const float max_hz = k_tone_max_ratio * m_sample_rate;
    const float cutoff = k_tone_min_hz * std::pow(max_hz / k_tone_min_hz, tone);
    const float g = std::tan(std::numbers::pi_v<float> * cutoff / m_sample_rate);
    return g / (1.0f + g);
}

}  // namespace thl::dsp::fx
