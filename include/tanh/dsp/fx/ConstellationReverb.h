#pragma once

#include <array>

#include <tanh/dsp/BaseProcessor.h>
#include <tanh/dsp/utils/BrownianNoise.h>
#include <tanh/dsp/utils/DynamicAllpass.h>
#include <tanh/dsp/utils/DynamicDelayLine.h>
#include <tanh/dsp/utils/FrequencyShifter.h>
#include <tanh/dsp/utils/PitchShifter.h>
#include <tanh/dsp/utils/SineOscillator.h>

namespace thl::dsp::fx {

enum class ConstellationReverbChannelMode : int {
    MonoToStereo = 0,  // Read ch0 as mono input  → stereo out on ch0+ch1
    StereoToStereo,    // Average ch0+ch1 as input → stereo out on ch0+ch1
    NUM_CHANNEL_MODES
};

/**
 * Constellation reverb processor — a Dattorro plate reverb with asymmetric
 * shimmer pitch shifting and per-side single-sideband frequency shifting.
 *
 * All delay memory is heap-allocated via DynamicDelayLine (AudioBuffer backed).
 * Internal modulation (LFO rates, Brownian rates, excursion depths) is
 * hardcoded to curated values. The public interface focuses on tonal shaping.
 *
 * The buffer must have at least 2 channels.
 * Reverb output is always written to ch0 (left) and ch1 (right).
 * Input mixing is controlled by the ChannelMode parameter:
 *   MonoToStereo  – read ch0 only
 *   StereoToStereo – average ch0+ch1
 *
 * Parameters:
 *   Decay              – reverb tail length [0, 1]
 *   Size               – tank scale factor [0.1, 4]
 *   Freeze             – infinite sustain, cuts input and damping (bool)
 *   PredelayMs         – input pre-delay in milliseconds [0, 200]
 *   Damping            – high-frequency absorption [0, 1]
 *   InputHpHz          – input highpass cutoff in Hz
 *   Shimmer            – shimmer pitch-shift wet mix [0, 1]
 *   ShimmerSemitones   – shimmer pitch interval in semitones
 *   ShimmerDetune      – asymmetric detune between L/R shimmer [-1, 1]
 *   ShimmerModDepth    – brownian pitch-mod depth in cents
 *   FreqShift          – frequency-shift wet mix [0, 1]
 *   FreqShiftHz        – base frequency shift in Hz
 *   FreqShiftDetune    – asymmetric detune between L/R shifter [-1, 1]
 *   FreqShiftModDepth  – brownian freq-mod depth in Hz
 *   ChannelModeParam   – ConstellationReverbChannelMode (int)
 */
class ConstellationReverbImpl : public thl::dsp::BaseProcessor {
public:
    ConstellationReverbImpl();
    ~ConstellationReverbImpl() override;

    void prepare(const double& sample_rate,
                 const size_t& samples_per_block,
                 const size_t& num_channels) override;

    void process(thl::dsp::audio::AudioBufferView buffer, uint32_t modulation_offset = 0) override;

protected:
    enum Parameter {
        Decay = 0,
        Size,
        Freeze,
        PredelayMs,
        Damping,
        InputHpHz,
        Shimmer,
        ShimmerSemitones,
        ShimmerDetune,
        ShimmerModDepth,
        FreqShift,
        FreqShiftHz,
        FreqShiftDetune,
        FreqShiftModDepth,
        ChannelModeParam,
        NUM_PARAMETERS
    };

    template <typename T>
    T get_parameter(Parameter p, uint32_t modulation_offset = 0);

    virtual float get_parameter_float(Parameter p, uint32_t modulation_offset = 0) = 0;
    virtual bool get_parameter_bool(Parameter p, uint32_t modulation_offset = 0) = 0;
    virtual int get_parameter_int(Parameter p, uint32_t modulation_offset = 0) = 0;

private:
    // ── Per-sample helpers ────────────────────────────────────────────────
    void process_sample(float in, float& left, float& right);
    void process_tank(float diffused, float& left, float& right);

    // ── Allocation / initialisation ───────────────────────────────────────
    void allocate_buffers(float predelay_ms);
    void prepare_oscillators();
    void apply_shimmer_pitch(uint32_t modulation_offset = 0);

    // ── Utility ───────────────────────────────────────────────────────────
    float sr_scale(int base) const;
    float scaled(int base) const;

    static std::pair<float, float> shimmer_detune_to_cents(float d);
    static std::pair<float, float> fshift_detune_to_hz(float d);

    std::array<utils::BrownianNoise*, 10> all_brownians();

    // ── Audio state ───────────────────────────────────────────────────────
    double m_sample_rate = 48000.0;
    float m_sr_ratio = 1.0f;
    float m_hp_coeff = 0.0f;
    float m_bw_coeff = 0.9995f;
    float m_damping_coeff = 0.9995f;
    float m_predelay_len = 0.0f;

    float m_hp_state = 0.0f;
    float m_bw_state = 0.0f;
    float m_damp_a = 0.0f;
    float m_damp_b = 0.0f;

    float m_size = 1.0f;
    float m_target_size = 1.0f;
    float m_size_smooth = 0.0f;
    float m_freq_shift = 0.0f;
    float m_target_fshift = 0.0f;
    float m_fshift_smooth = 0.0f;

    float m_tank_a_out = 0.0f;
    float m_tank_b_out = 0.0f;

    // ── Cached per-block parameters (updated once per block in process()) ──
    // Avoids virtual dispatch inside the per-sample processing loop.
    float m_p_decay = 0.85f;
    bool m_p_freeze = false;
    float m_p_shimmer = 0.0f;
    float m_p_shim_mod = 0.0f;
    float m_p_fshift_hz = 132.0f;
    float m_p_fshift_det = 0.2f;
    float m_p_fshift_mod = 40.0f;

    // ── Input stage ───────────────────────────────────────────────────────
    utils::DynamicAllpass m_input_ap[4];
    utils::DynamicDelayLine m_predelay;

    // ── Tank — half A ─────────────────────────────────────────────────────
    utils::DynamicAllpass m_ap_a1, m_ap_a2;
    utils::DynamicDelayLine m_delay_a1, m_delay_a2;

    // ── Tank — half B ─────────────────────────────────────────────────────
    utils::DynamicAllpass m_ap_b1, m_ap_b2;
    utils::DynamicDelayLine m_delay_b1, m_delay_b2;

    // ── LFOs ──────────────────────────────────────────────────────────────
    utils::SineOscillator m_lfo_a, m_lfo_b;

    // ── Brownian noise generators ─────────────────────────────────────────
    utils::BrownianNoise m_brown_exc_a, m_brown_exc_b;
    utils::BrownianNoise m_brown_delay_a1, m_brown_delay_a2;
    utils::BrownianNoise m_brown_delay_b1, m_brown_delay_b2;
    utils::BrownianNoise m_brown_ap_a2, m_brown_ap_b2;
    utils::BrownianNoise m_brown_shim_a, m_brown_shim_b;
    utils::BrownianNoise m_brown_fshift_a, m_brown_fshift_b;

    // ── Shimmer ───────────────────────────────────────────────────────────
    utils::PitchShifter m_pitch_a, m_pitch_b;

    // ── Frequency shifters ────────────────────────────────────────────────
    utils::FrequencyShifter m_fshift_a, m_fshift_b;
};

template <>
inline float ConstellationReverbImpl::get_parameter<float>(Parameter p,
                                                           uint32_t modulation_offset) {
    return get_parameter_float(p, modulation_offset);
}
template <>
inline bool ConstellationReverbImpl::get_parameter<bool>(Parameter p, uint32_t modulation_offset) {
    return get_parameter_bool(p, modulation_offset);
}
template <>
inline int ConstellationReverbImpl::get_parameter<int>(Parameter p, uint32_t modulation_offset) {
    return get_parameter_int(p, modulation_offset);
}

}  // namespace thl::dsp::fx
