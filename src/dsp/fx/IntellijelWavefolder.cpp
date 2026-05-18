#include <tanh/dsp/fx/IntellijelWavefolder.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>

#include "tanh/dsp/audio/AudioBufferView.h"

namespace thl::dsp::fx {

namespace {
// 5 ms linear ramp on Folds — long enough to smear control-rate step changes
// below the audible click threshold, short enough that intended modulation
// still tracks.
constexpr double k_param_smoothing_time = 0.005;

// Drive / JfetTone / Symmetry use a longer ramp than Folds because they
// produce the most aggressive timbre changes when stepped. With output-level
// normalization in place, the loudness no longer jumps on randomize, so 30 ms
// is sufficient — long enough that the harmonic sweep stays gentle, short
// enough that LFO modulation up to ~10 Hz still tracks usefully.
constexpr double k_drive_smoothing_time = 0.030;
constexpr double k_jfet_tone_smoothing_time = 0.030;
constexpr double k_symmetry_smoothing_time = 0.030;

// One-pole DC blocker corner — 30 Hz removes the sustained DC the transfer
// function produces for non-zero Symmetry and also rejects most of the
// low-frequency drift during a Symmetry parameter ramp (a 120 ms ramp's
// spectral peak sits around 4 Hz, where a 30 Hz corner attenuates ~87%).
// 30 Hz is still well below the audible bass range post-wavefolder, where
// the transfer function generates its own harmonic content.
constexpr float k_dc_blocker_cutoff_hz = 30.0f;

// One-pole envelope-follower time constant on |input| and |output|. 10 ms
// is short enough that the normalization tracks transients without smearing
// them, long enough that it doesn't ride out every sample.
constexpr double k_envelope_follower_time = 0.010;

// Floor on the output envelope used in the normalization divisor — prevents
// divide-by-zero blow-ups when the wavefolder output is momentarily silent.
constexpr float k_output_env_floor = 1e-6f;

// Cap on the makeup gain applied to the output. Without this, an input
// transient over a momentarily-quiet wavefolder output could amplify
// arbitrarily. 4× = 12 dB is plenty of headroom for any legitimate
// input/output mismatch.
constexpr float k_max_makeup_gain = 4.0f;
}  // namespace

IntellijelWavefolderImpl::IntellijelWavefolderImpl() = default;
IntellijelWavefolderImpl::~IntellijelWavefolderImpl() = default;

void IntellijelWavefolderImpl::prepare(const double& sample_rate,
                                       const size_t& /*samples_per_block*/,
                                       const size_t& num_channels) {
    m_smoothed_drive.reset(sample_rate, k_drive_smoothing_time);
    m_smoothed_folds.reset(sample_rate, k_param_smoothing_time);
    m_smoothed_symmetry.reset(sample_rate, k_symmetry_smoothing_time);
    m_smoothed_jfet_tone.reset(sample_rate, k_jfet_tone_smoothing_time);

    // Prime with current parameter values so prepare doesn't introduce a ramp
    // from zero on the first block.
    const float drive = std::clamp(get_parameter<float>(Drive), 0.1f, 20.0f);
    const float folds = std::clamp(get_parameter<float>(Folds), 0.0f, 10.0f);
    const float symmetry = get_parameter<float>(Symmetry);
    const float jfet_tone = std::clamp(get_parameter<float>(JfetTone), 0.0f, 1.0f);

    m_smoothed_drive.set_current_and_target_value(drive);
    m_smoothed_folds.set_current_and_target_value(folds);
    m_smoothed_symmetry.set_current_and_target_value(symmetry);
    m_smoothed_jfet_tone.set_current_and_target_value(jfet_tone);

    // Prime the DC blocker's x_prev with the transfer-function output for a
    // zero input. A silent input then yields zero output from sample 0,
    // instead of a 16 ms decay from the static DC level the transfer function
    // synthesises for non-zero Symmetry.
    const float dc_steady = process_sample(0.0f, drive, folds, symmetry, jfet_tone);
    m_dc_x_prev.assign(num_channels, dc_steady);
    m_dc_y_prev.assign(num_channels, 0.0f);
    m_dc_pole = std::exp(-2.0f * std::numbers::pi_v<float> * k_dc_blocker_cutoff_hz /
                         static_cast<float>(sample_rate));

    // Envelope-follower pole — one-pole LP on |sample|, decay constant set
    // by k_envelope_follower_time. Start the followers from zero so the very
    // first input ramps up the normalization naturally instead of jumping.
    m_env_pole = std::exp(
        -1.0f / static_cast<float>(k_envelope_follower_time * static_cast<float>(sample_rate)));
    m_input_env.assign(num_channels, 0.0f);
    m_output_env.assign(num_channels, 0.0f);
}

void IntellijelWavefolderImpl::process(thl::dsp::audio::AudioBufferView buffer,
                                       uint32_t modulation_offset) {
    m_smoothed_drive.set_target_value(
        std::clamp(get_parameter<float>(Drive, modulation_offset), 0.1f, 20.0f));
    m_smoothed_folds.set_target_value(
        std::clamp(get_parameter<float>(Folds, modulation_offset), 0.0f, 10.0f));
    m_smoothed_symmetry.set_target_value(get_parameter<float>(Symmetry, modulation_offset));
    m_smoothed_jfet_tone.set_target_value(
        std::clamp(get_parameter<float>(JfetTone, modulation_offset), 0.0f, 1.0f));

    const size_t num_channels = std::min(buffer.get_num_channels(), m_dc_x_prev.size());
    const size_t num_frames = buffer.get_num_frames();

    for (size_t i = 0; i < num_frames; ++i) {
        const float drive = m_smoothed_drive.get_smoothed_value(1);
        const float folds = m_smoothed_folds.get_smoothed_value(1);
        const float symmetry = m_smoothed_symmetry.get_smoothed_value(1);
        const float jfet_tone = m_smoothed_jfet_tone.get_smoothed_value(1);

        for (size_t ch = 0; ch < num_channels; ++ch) {
            float* data = buffer.get_write_pointer(ch);
            const float in_sample = data[i];
            const float xn = process_sample(in_sample, drive, folds, symmetry, jfet_tone);
            const float yn = xn - m_dc_x_prev[ch] + m_dc_pole * m_dc_y_prev[ch];
            m_dc_x_prev[ch] = xn;
            m_dc_y_prev[ch] = yn;

            // One-pole envelope followers on |input| and |post-fold output|.
            const float abs_in = std::fabs(in_sample);
            m_input_env[ch] = abs_in + m_env_pole * (m_input_env[ch] - abs_in);
            const float abs_out = std::fabs(yn);
            m_output_env[ch] = abs_out + m_env_pole * (m_output_env[ch] - abs_out);

            // Scale output so its envelope tracks the input envelope. Floor
            // the divisor and cap the gain to avoid blow-ups on transient
            // mismatches.
            const float scale =
                std::min(m_input_env[ch] / std::max(m_output_env[ch], k_output_env_floor),
                         k_max_makeup_gain);
            data[i] = yn * scale;
        }
    }
}

float IntellijelWavefolderImpl::process_sample(float x,
                                               float drive,
                                               float folds,
                                               float symmetry,
                                               float jfet_tone) {
    float s = (x + symmetry) * drive;
    s = std::sin(s * (1.0f + folds));

    if (jfet_tone > 0.0f) {
        const float saturated = jfet_saturate(s);
        s = s * (1.0f - jfet_tone) + saturated * jfet_tone;
    }
    return s;
}

float IntellijelWavefolderImpl::jfet_saturate(float x) {
    const float sym = std::tanh(x * 1.5f);
    return x > 0.0f ? sym * 0.95f : sym;
}

}  // namespace thl::dsp::fx
