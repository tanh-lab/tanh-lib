#pragma once

#include <tanh/core/BufferView.h>
#include <tanh/dsp/DspTypes.h>
#include <tanh/dsp/filter/OnePole.h>
#include <tanh/dsp/filter/Svf.h>
#include <tanh/dsp/utils/SmoothedValue.h>

#include <algorithm>
#include <array>
#include <cstddef>

namespace thl::dsp::filter {

// Click-free morphing synth filter: a one-pole plus a cascade of TPT SVF
// stages, with per-sample smoothed coefficients and output-tap crossfades.
//
//   frequency -- cutoff as normalised f/fs, prewarped per sample
//   q         -- resonance of the first SVF stage; later stages run at fixed
//                Butterworth damping so the peak does not compound (the 6 dB
//                one-pole tap has no resonance)
//   mode      -- 0 = low-pass .. 1 = high-pass (per-stage output blend)
//   slope     -- 0..1 across the taps 6, 12, 24, 36, .., 96 dB/oct
//                (linear in tap index; use slope_for_tap() to address one)
//
// Every parameter ramps linearly over the smoothing time, so mode/slope
// toggles and fast frequency/Q sweeps produce no discontinuities. Only the
// stages the current slope needs are processed; a stage re-entering the mix
// is state-reset and fades in from zero gain. As in analogue cascades, each
// Butterworth stage contributes -3 dB at the cutoff, so high slopes read the
// cutoff as a progressively deeper knee.
class MorphFilter {
public:
    static constexpr size_t k_num_svf_stages = 8;               // 12 .. 96 dB/oct
    static constexpr size_t k_num_taps = k_num_svf_stages + 1;  // + the 6 dB one-pole

    static constexpr double k_default_smoothing_seconds = 0.005;
    static constexpr float k_min_frequency = 1.0e-5f;
    static constexpr float k_max_frequency = 0.497f;
    static constexpr float k_min_q = 0.1f;
    static constexpr float k_max_q = 40.0f;
    static constexpr float k_butterworth_q = 0.70710678f;

    // Slope value that lands exactly on a tap: 0 = 6 dB/oct one-pole,
    // 1 = 12 dB/oct, 2 = 24 dB/oct, .., k_num_svf_stages = 96 dB/oct.
    static constexpr float slope_for_tap(size_t tap) {
        return static_cast<float>(tap) / static_cast<float>(k_num_taps - 1);
    }

    // Must be called before processing. Snaps every parameter to its current
    // target and clears the filter state.
    void prepare(double sample_rate, double smoothing_seconds = k_default_smoothing_seconds) {
        m_frequency.reset(sample_rate, smoothing_seconds);
        m_q.reset(sample_rate, smoothing_seconds);
        m_mode.reset(sample_rate, smoothing_seconds);
        m_slope.reset(sample_rate, smoothing_seconds);
        snap();
        reset();
    }

    void reset() {
        m_one_pole.reset();
        for (auto& stage : m_stages) { stage.reset(); }
        m_active_svf_stages = 0;
        m_one_pole_active = false;
    }

    // Jump every smoothed parameter to its target (e.g. on preset load).
    void snap() {
        m_frequency.set_current_and_target_value(m_target_frequency);
        m_q.set_current_and_target_value(m_target_q);
        m_mode.set_current_and_target_value(m_target_mode);
        m_slope.set_current_and_target_value(m_target_slope);
    }

    // Cutoff as normalised frequency f/fs (clamped to [k_min, k_max]).
    void set_frequency(float f) {
        m_target_frequency = std::clamp(f, k_min_frequency, k_max_frequency);
        m_frequency.set_target_value(m_target_frequency);
    }

    void set_q(float q) {
        m_target_q = std::clamp(q, k_min_q, k_max_q);
        m_q.set_target_value(m_target_q);
    }

    // 0 = low-pass, 1 = high-pass; intermediate values blend the taps.
    void set_mode(float mode) {
        m_target_mode = std::clamp(mode, 0.0f, 1.0f);
        m_mode.set_target_value(m_target_mode);
    }

    // 0..1 across the slope taps; intermediate values blend adjacent taps.
    void set_slope(float slope) {
        m_target_slope = std::clamp(slope, 0.0f, 1.0f);
        m_slope.set_target_value(m_target_slope);
    }

    float process(float in) {
        const float f = m_frequency.get_smoothed_value(1);
        const float q = m_q.get_smoothed_value(1);
        const float mode = m_mode.get_smoothed_value(1);
        const float slope = m_slope.get_smoothed_value(1);

        // The smoothed slope can over/undershoot its clamped target by float
        // rounding mid-ramp — without this clamp, hi indexes past m_stages.
        const float position = std::clamp(slope, 0.0f, 1.0f) * static_cast<float>(k_num_taps - 1);
        const auto lo = static_cast<size_t>(position);
        const float frac = position - static_cast<float>(lo);
        const size_t hi = frac > 0.0f ? lo + 1 : lo;

        const float g = OnePole::tan<Approximation::Fast>(f);

        float y_lo = 0.0f;
        if (lo == 0) {
            if (!m_one_pole_active) {
                m_one_pole.reset();
                m_one_pole_active = true;
            }
            m_one_pole.set_g(g);
            const float lp = m_one_pole.process<FilterMode::LowPass>(in);
            y_lo = lp + mode * (in - 2.0f * lp);  // blend lp .. hp = in - lp
        } else {
            m_one_pole_active = false;
        }

        if (hi > m_active_svf_stages) {
            for (size_t k = m_active_svf_stages; k < hi; ++k) { m_stages[k].reset(); }
        }
        m_active_svf_stages = hi;

        float y_hi = y_lo;
        float x = in;
        for (size_t k = 1; k <= hi; ++k) {
            m_stages[k - 1].set_g_q(g, k == 1 ? q : k_butterworth_q);
            const auto taps = m_stages[k - 1].process_all(x);
            x = taps.m_lp + mode * (taps.m_hp - taps.m_lp);
            if (k == lo) { y_lo = x; }
            if (k == hi) { y_hi = x; }
        }

        return y_lo + frac * (y_hi - y_lo);
    }

    void process(const thl::core::ConstBufferView& in, thl::core::BufferView out) {
        const float* const in_ptr = in.get_read_pointer(0);
        float* const out_ptr = out.get_write_pointer(0);
        const size_t size = in.get_num_samples();
        for (size_t i = 0; i < size; ++i) { out_ptr[i] = process(in_ptr[i]); }
    }

private:
    OnePole m_one_pole;
    std::array<Svf, k_num_svf_stages> m_stages;
    size_t m_active_svf_stages = 0;
    bool m_one_pole_active = false;

    utils::SmoothedValue m_frequency;
    utils::SmoothedValue m_q;
    utils::SmoothedValue m_mode;
    utils::SmoothedValue m_slope;

    float m_target_frequency = 0.25f;
    float m_target_q = k_butterworth_q;
    float m_target_mode = 0.0f;
    float m_target_slope = slope_for_tap(1);
};

}  // namespace thl::dsp::filter
