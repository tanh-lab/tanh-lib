#include <tanh/dsp/fx/ConstellationReverb.h>

#include <algorithm>
#include <cmath>
#include <numbers>

#include <tanh/dsp/utils/DspMath.h>

namespace thl::dsp::fx {

// ── Dattorro base delay values (original SR = 29761 Hz) ──────────────────────

static constexpr float k_original_sr = 29761.0f;
static constexpr float k_bandwidth = 0.9995f;
static constexpr float k_input_dif_f1 = 0.75f;
static constexpr float k_input_dif_f2 = 0.625f;
static constexpr float k_decay_dif_f1 = 0.70f;
static constexpr float k_decay_dif_f2 = 0.50f;
static constexpr float k_mod_rate_a = 0.5f;
static constexpr float k_mod_rate_b = 0.3f;
static constexpr float k_mod_excursion = 25.0f;
static constexpr float k_exc_brown_rate = 3.7f;
static constexpr float k_exc_brown_depth = 28.0f;
static constexpr float k_shim_brown_rate = 9.1f;
static constexpr float k_fshift_brown_rate = 5.3f;
static constexpr float k_ap_mod_depth = 3.0f;
static constexpr float k_delay_mod_depth = 3.0f;
static constexpr float k_max_size = 4.0f;
static constexpr float k_max_predelay_ms = 200.0f;

static constexpr int k_base_input_ap[4] = {142, 107, 379, 277};
static constexpr int k_base_ap_a1 = 672;
static constexpr int k_base_delay_a1 = 4453;
static constexpr int k_base_ap_a2 = 1800;
static constexpr int k_base_delay_a2 = 3720;
static constexpr int k_base_ap_b1 = 908;
static constexpr int k_base_delay_b1 = 4217;
static constexpr int k_base_ap_b2 = 2656;
static constexpr int k_base_delay_b2 = 3163;

static constexpr int k_base_taps_l[7] = {266, 2974, 1913, 1996, 1990, 187, 1066};
static constexpr int k_base_taps_r[7] = {353, 3627, 1228, 2673, 2111, 335, 121};

// ── Constructor / destructor ──────────────────────────────────────────────────

ConstellationReverbImpl::ConstellationReverbImpl() = default;
ConstellationReverbImpl::~ConstellationReverbImpl() = default;

// ── BaseProcessor interface ───────────────────────────────────────────────────

void ConstellationReverbImpl::prepare(const double& sample_rate,
                                      const size_t& /*samples_per_block*/,
                                      const size_t& /*num_channels*/) {
    m_sample_rate = sample_rate;
    m_sr_ratio = static_cast<float>(sample_rate) / k_original_sr;

    const float sr = static_cast<float>(sample_rate);
    const float pi2 = 2.0f * std::numbers::pi_v<float>;

    m_damping_coeff = 1.0f - get_parameter<float>(Damping);
    m_hp_coeff = 1.0f - std::exp(-pi2 * get_parameter<float>(InputHpHz) / sr);
    m_bw_coeff = k_bandwidth;
    m_size_smooth = 1.0f - std::exp(-pi2 * 0.3f / sr);
    m_fshift_smooth = 1.0f - std::exp(-pi2 * 0.3f / sr);

    m_size = get_parameter<float>(Size);
    m_target_size = m_size;
    m_freq_shift = get_parameter<float>(FreqShift);
    m_target_fshift = m_freq_shift;

    allocate_buffers(get_parameter<float>(PredelayMs));
    prepare_oscillators();

    m_hp_state = m_bw_state = 0.0f;
    m_damp_a = m_damp_b = 0.0f;
    m_tank_a_out = m_tank_b_out = 0.0f;
}

void ConstellationReverbImpl::process(thl::dsp::audio::AudioBufferView buffer,
                                      uint32_t modulation_offset) {
    if (buffer.get_num_channels() < 2) { return; }

    const size_t num_frames = buffer.get_num_frames();
    float* ch0 = buffer.get_write_pointer(0);
    float* ch1 = buffer.get_write_pointer(1);

    // ── Cache all parameters once per block ───────────────────────────────
    // These are stored as members so process_tank() can use them without
    // incurring a virtual dispatch on every sample.
    m_p_decay = get_parameter<float>(Decay, modulation_offset);
    m_p_freeze = get_parameter<bool>(Freeze, modulation_offset);
    m_p_shimmer = get_parameter<float>(Shimmer, modulation_offset);
    m_p_shim_mod = get_parameter<float>(ShimmerModDepth, modulation_offset);
    m_p_fshift_hz = get_parameter<float>(FreqShiftHz, modulation_offset);
    m_p_fshift_det = get_parameter<float>(FreqShiftDetune, modulation_offset);
    m_p_fshift_mod = get_parameter<float>(FreqShiftModDepth, modulation_offset);

    m_damping_coeff = 1.0f - get_parameter<float>(Damping, modulation_offset);
    m_predelay_len = get_parameter<float>(PredelayMs, modulation_offset) *
                     static_cast<float>(m_sample_rate) / 1000.0f;
    m_hp_coeff = 1.0f - std::exp(-2.0f * std::numbers::pi_v<float> *
                                 get_parameter<float>(InputHpHz, modulation_offset) /
                                 static_cast<float>(m_sample_rate));
    m_target_size = get_parameter<float>(Size, modulation_offset);
    m_target_fshift = get_parameter<float>(FreqShift, modulation_offset);

    apply_shimmer_pitch(modulation_offset);

    const auto mode = static_cast<ConstellationReverbChannelMode>(
        get_parameter<int>(ChannelModeParam, modulation_offset));

    // ── Per-sample loop ───────────────────────────────────────────────────
    for (size_t i = 0; i < num_frames; ++i) {
        float in = 0.0f;
        switch (mode) {
            case ConstellationReverbChannelMode::StereoToStereo:
                in = (ch0[i] + ch1[i]) * 0.5f;
                break;
            case ConstellationReverbChannelMode::MonoToStereo:
            default: in = ch0[i]; break;
        }

        float left = 0.0f, right = 0.0f;
        process_sample(in, left, right);
        ch0[i] = left;
        ch1[i] = right;
    }
}

// ── Per-sample processing ─────────────────────────────────────────────────────

void ConstellationReverbImpl::process_sample(float x, float& left, float& right) {
    // Input highpass: subtract LP to remove sub-bass
    utils::one_pole(m_hp_state, x, m_hp_coeff);
    x -= m_hp_state;

    x = m_predelay.write_read(x, m_predelay_len);

    utils::one_pole(m_bw_state, x, m_bw_coeff);
    x = m_bw_state;

    x = m_input_ap[0].process(x, sr_scale(k_base_input_ap[0]), k_input_dif_f1);
    x = m_input_ap[1].process(x, sr_scale(k_base_input_ap[1]), k_input_dif_f1);
    x = m_input_ap[2].process(x, sr_scale(k_base_input_ap[2]), k_input_dif_f2);
    x = m_input_ap[3].process(x, sr_scale(k_base_input_ap[3]), k_input_dif_f2);

    process_tank(x, left, right);
}

void ConstellationReverbImpl::process_tank(float diffused, float& left, float& right) {
    // Smooth size and freq_shift toward their per-block targets (~0.3 Hz LP)
    utils::one_pole(m_size, m_target_size, m_size_smooth);
    utils::one_pole(m_freq_shift, m_target_fshift, m_fshift_smooth);

    // ── Excursion: LFO + Brownian ─────────────────────────────────────────
    const float lfo_a = m_lfo_a.process();
    const float lfo_b = m_lfo_b.process();
    const float brown_a = m_brown_exc_a.process() * k_exc_brown_depth;
    const float brown_b = m_brown_exc_b.process() * k_exc_brown_depth;

    const float ap_mod = k_ap_mod_depth * m_sr_ratio;
    const float delay_mod = k_delay_mod_depth * m_sr_ratio;

    // ── Freq-shift modulation (per-side brownian + detune) ─────────────────
    const float fbrown_a = m_brown_fshift_a.process();
    const float fbrown_b = m_brown_fshift_b.process();
    if (m_freq_shift > 0.0f) {
        const auto [dha, dhb] = fshift_detune_to_hz(m_p_fshift_det);
        m_fshift_a.set_shift(m_p_fshift_hz + dha + fbrown_a * m_p_fshift_mod);
        m_fshift_b.set_shift(m_p_fshift_hz + dhb + fbrown_b * m_p_fshift_mod);
    }

    const float eff_decay = m_p_freeze ? 1.0f : m_p_decay;
    const float input_gain = m_p_freeze ? 0.0f : 1.0f;

    // ── Cross-coupled feedback (freq-shift ramps in above 0.5) ────────────
    float a_fb = eff_decay * m_tank_b_out;
    float b_fb = eff_decay * m_tank_a_out;
    if (m_freq_shift > 0.5f) {
        const float cross = (m_freq_shift - 0.5f) * 2.0f;
        a_fb = (1.0f - cross) * a_fb + cross * m_fshift_a.process(a_fb);
        b_fb = (1.0f - cross) * b_fb + cross * m_fshift_b.process(b_fb);
    }
    const float a_in = input_gain * diffused + std::tanh(a_fb);
    const float b_in = input_gain * diffused + std::tanh(b_fb);

    // ── Half A ────────────────────────────────────────────────────────────
    float x_a = m_ap_a1.process(a_in, scaled(k_base_ap_a1) + lfo_a + brown_a, k_decay_dif_f1);
    x_a = m_delay_a1.write_read(x_a,
                                scaled(k_base_delay_a1) + m_brown_delay_a1.process() * delay_mod);
    if (!m_p_freeze) {
        utils::one_pole(m_damp_a, x_a, m_damping_coeff);
        x_a = m_damp_a;
    }
    x_a = std::tanh(x_a * eff_decay);
    if (m_freq_shift > 0.0f) {
        x_a = (1.0f - m_freq_shift) * x_a + m_freq_shift * m_fshift_a.process(x_a);
    }
    x_a = m_ap_a2.process(x_a,
                          scaled(k_base_ap_a2) + m_brown_ap_a2.process() * ap_mod,
                          k_decay_dif_f2);
    float tank_a =
        m_delay_a2.write_read(x_a,
                              scaled(k_base_delay_a2) + m_brown_delay_a2.process() * delay_mod);
    if (m_p_shimmer > 0.0f) {
        m_pitch_a.set_cents_modulation(m_brown_shim_a.process() * m_p_shim_mod);
        tank_a = (1.0f - m_p_shimmer) * tank_a + m_p_shimmer * m_pitch_a.process(tank_a);
    }
    m_tank_a_out = tank_a;

    // ── Half B ────────────────────────────────────────────────────────────
    float x_b = m_ap_b1.process(b_in, scaled(k_base_ap_b1) + lfo_b + brown_b, k_decay_dif_f1);
    x_b = m_delay_b1.write_read(x_b,
                                scaled(k_base_delay_b1) + m_brown_delay_b1.process() * delay_mod);
    if (!m_p_freeze) {
        utils::one_pole(m_damp_b, x_b, m_damping_coeff);
        x_b = m_damp_b;
    }
    x_b = std::tanh(x_b * eff_decay);
    if (m_freq_shift > 0.0f) {
        x_b = (1.0f - m_freq_shift) * x_b + m_freq_shift * m_fshift_b.process(x_b);
    }
    x_b = m_ap_b2.process(x_b,
                          scaled(k_base_ap_b2) + m_brown_ap_b2.process() * ap_mod,
                          k_decay_dif_f2);
    float tank_b =
        m_delay_b2.write_read(x_b,
                              scaled(k_base_delay_b2) + m_brown_delay_b2.process() * delay_mod);
    if (m_p_shimmer > 0.0f) {
        m_pitch_b.set_cents_modulation(m_brown_shim_b.process() * m_p_shim_mod);
        tank_b = (1.0f - m_p_shimmer) * tank_b + m_p_shimmer * m_pitch_b.process(tank_b);
    }
    m_tank_b_out = tank_b;

    // ── Output tap matrix (7-tap per side, gain 0.6) ──────────────────────
    left = 0.6f *
           (m_delay_a1.tap(scaled(k_base_taps_l[0])) + m_delay_a1.tap(scaled(k_base_taps_l[1])) -
            m_ap_a2.tap(scaled(k_base_taps_l[2])) + m_delay_a2.tap(scaled(k_base_taps_l[3])) -
            m_delay_b1.tap(scaled(k_base_taps_l[4])) - m_ap_b2.tap(scaled(k_base_taps_l[5])) -
            m_delay_b2.tap(scaled(k_base_taps_l[6])));

    right = 0.6f *
            (m_delay_b1.tap(scaled(k_base_taps_r[0])) + m_delay_b1.tap(scaled(k_base_taps_r[1])) -
             m_ap_b2.tap(scaled(k_base_taps_r[2])) + m_delay_b2.tap(scaled(k_base_taps_r[3])) -
             m_delay_a1.tap(scaled(k_base_taps_r[4])) - m_ap_a2.tap(scaled(k_base_taps_r[5])) -
             m_delay_a2.tap(scaled(k_base_taps_r[6])));
}

// ── Allocation / initialisation ───────────────────────────────────────────────

void ConstellationReverbImpl::allocate_buffers(float predelay_ms) {
    const float sr = m_sr_ratio;

    const int exc_headroom =
        static_cast<int>(std::ceil(k_mod_excursion * sr)) + static_cast<int>(k_exc_brown_depth) + 8;
    const int brown_headroom = static_cast<int>(std::ceil(3.0f * sr)) + 4;

    auto tank_buf = [&](int base, int max_tap, int headroom) -> size_t {
        return static_cast<size_t>(std::ceil(std::max(base, max_tap) * sr * k_max_size)) +
               static_cast<size_t>(headroom) + 1;
    };
    auto input_buf = [&](int base) -> size_t {
        return static_cast<size_t>(std::ceil(base * sr)) + 1;
    };

    for (int i = 0; i < 4; ++i) { m_input_ap[i].prepare(input_buf(k_base_input_ap[i])); }

    m_predelay.prepare(static_cast<size_t>(std::ceil(k_max_predelay_ms *
                                                     static_cast<float>(m_sample_rate) / 1000.0f)) +
                       1);
    m_predelay_len = predelay_ms * static_cast<float>(m_sample_rate) / 1000.0f;

    m_ap_a1.prepare(tank_buf(k_base_ap_a1, 0, exc_headroom));
    m_delay_a1.prepare(tank_buf(k_base_delay_a1, 2974, brown_headroom));
    m_ap_a2.prepare(tank_buf(k_base_ap_a2, 1913, brown_headroom));
    m_delay_a2.prepare(tank_buf(k_base_delay_a2, 1996, brown_headroom));

    m_ap_b1.prepare(tank_buf(k_base_ap_b1, 0, exc_headroom));
    m_delay_b1.prepare(tank_buf(k_base_delay_b1, 3627, brown_headroom));
    m_ap_b2.prepare(tank_buf(k_base_ap_b2, 1228, brown_headroom));
    m_delay_b2.prepare(tank_buf(k_base_delay_b2, 2673, brown_headroom));

    m_pitch_a.prepare();
    m_pitch_b.prepare();
}

void ConstellationReverbImpl::prepare_oscillators() {
    const float sr = static_cast<float>(m_sample_rate);
    const float exc = k_mod_excursion * m_sr_ratio;

    m_lfo_a.prepare(k_mod_rate_a, sr, exc);
    m_lfo_b.prepare(k_mod_rate_b, sr, exc);

    m_brown_exc_a.prepare(k_exc_brown_rate, sr);
    m_brown_exc_b.prepare(k_exc_brown_rate, sr);
    m_brown_delay_a1.prepare(1.3f, sr);
    m_brown_delay_a2.prepare(0.7f, sr);
    m_brown_delay_b1.prepare(1.9f, sr);
    m_brown_delay_b2.prepare(0.5f, sr);
    m_brown_ap_a2.prepare(2.3f, sr);
    m_brown_ap_b2.prepare(1.1f, sr);
    m_brown_shim_a.prepare(k_shim_brown_rate, sr);
    m_brown_shim_b.prepare(k_shim_brown_rate, sr);
    m_brown_fshift_a.prepare(k_fshift_brown_rate, sr);
    m_brown_fshift_b.prepare(k_fshift_brown_rate, sr);

    const float fshift_hz = get_parameter<float>(FreqShiftHz);
    const auto [ha, hb] = fshift_detune_to_hz(get_parameter<float>(FreqShiftDetune));
    m_fshift_a.prepare(fshift_hz + ha, sr);
    m_fshift_b.prepare(fshift_hz + hb, sr);

    apply_shimmer_pitch(0);
}

void ConstellationReverbImpl::apply_shimmer_pitch(uint32_t modulation_offset) {
    const auto [ca, cb] =
        shimmer_detune_to_cents(get_parameter<float>(ShimmerDetune, modulation_offset));
    m_pitch_a.set_pitch(get_parameter<float>(ShimmerSemitones, modulation_offset), ca);
    m_pitch_b.set_pitch(get_parameter<float>(ShimmerSemitones, modulation_offset), cb);
}

// ── Utilities ─────────────────────────────────────────────────────────────────

float ConstellationReverbImpl::sr_scale(int base) const {
    return std::max(1.0f, std::round(static_cast<float>(base) * m_sr_ratio));
}

float ConstellationReverbImpl::scaled(int base) const {
    return std::max(1.0f, static_cast<float>(base) * m_sr_ratio * m_size);
}

std::pair<float, float> ConstellationReverbImpl::shimmer_detune_to_cents(float d) {
    if (d >= 0.0f) { return {-40.0f * d, 100.0f * d}; }
    return {-100.0f * d, 60.0f * d};
}

std::pair<float, float> ConstellationReverbImpl::fshift_detune_to_hz(float d) {
    if (d >= 0.0f) { return {400.0f * d, -160.0f * d}; }
    return {240.0f * d, -400.0f * d};
}

std::array<utils::BrownianNoise*, 10> ConstellationReverbImpl::all_brownians() {
    return {&m_brown_exc_a,
            &m_brown_exc_b,
            &m_brown_delay_a1,
            &m_brown_delay_a2,
            &m_brown_delay_b1,
            &m_brown_delay_b2,
            &m_brown_ap_a2,
            &m_brown_ap_b2,
            &m_brown_shim_a,
            &m_brown_shim_b};
}

}  // namespace thl::dsp::fx
