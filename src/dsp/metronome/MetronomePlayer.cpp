#include <tanh/dsp/metronome/MetronomePlayer.h>
#include <tanh/dsp/audio/AudioBufferView.h>
#include <tanh/dsp/transport/TransportClock.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>

namespace thl::dsp::metronome {

// ── Voice ─────────────────────────────────────────────────────────────────────

void MetronomePlayerImpl::Voice::trigger(float sample_rate,
                                         float in_freq,
                                         float in_gain,
                                         float release_ms) {
    m_phase = 0.0f;
    m_freq = in_freq;
    m_gain = in_gain;

    m_env.set_sample_rate(sample_rate);
    m_env.set_parameters(k_attack_ms, k_attack_ms, 1.0f, release_ms, 0.0f, 0.0f, 1.0f);
    m_env.reset();
    m_env.note_on();
}

float MetronomePlayerImpl::Voice::tick(float sample_rate) {
    if (!m_env.is_active()) { return 0.0f; }

    if (m_env.get_state() == thl::dsp::utils::ADSR::State::SUSTAIN) { m_env.note_off(); }

    const float envelope = m_env.process();
    const float sample = std::sin(m_phase * 2.0f * std::numbers::pi_v<float>);
    m_phase += m_freq / sample_rate;
    if (m_phase >= 1.0f) { m_phase -= 1.0f; }
    return m_gain * envelope * sample;
}

// ── MetronomePlayer ───────────────────────────────────────────────────────────

MetronomePlayerImpl::MetronomePlayerImpl(transport::TransportClock& clock) : m_clock(clock) {}

void MetronomePlayerImpl::prepare(const double& sample_rate,
                                  const size_t& /*samples_per_block*/,
                                  const size_t& /*num_channels*/) {
    m_sample_rate = sample_rate;
}

void MetronomePlayerImpl::process(thl::dsp::audio::AudioBufferView buffer,
                                  uint32_t modulation_offset) {
    const auto frame_count = static_cast<uint32_t>(buffer.get_num_frames());
    const auto num_channels = buffer.get_num_channels();
    const bool enabled = get_parameter_bool(Enabled, modulation_offset);
    const bool playing = enabled && m_clock.is_playing();

    const auto sig_num = m_clock.sig_num();
    const auto sig_denom = m_clock.sig_denom();
    const double bar_size =
        transport::beats_per_division(transport::Division::Bar, sig_num, sig_denom);
    const double bps = m_clock.bpm() / (60.0 * m_sample_rate);
    const double beat_start = m_clock.beat_at_sample(0);

    const auto rhythm = transport::division_from_int(get_parameter_int(Rhythm, modulation_offset));
    const double div_size = transport::beats_per_division(rhythm, sig_num, sig_denom);

    const auto bar_off =
        playing ? crossing_offset(bar_size, beat_start, bps, frame_count) : std::nullopt;
    const auto div_off = (playing && div_size != bar_size)
                             ? crossing_offset(div_size, beat_start, bps, frame_count)
                             : std::nullopt;

    for (uint32_t i = 0; i < frame_count; ++i) {
        if (bar_off && i == *bar_off) {
            trigger_accent();
        } else if (div_off && i == *div_off) {
            trigger_click();
        }

        const float s = tick_voices();
        for (size_t ch = 0; ch < num_channels; ++ch) { buffer.get_write_pointer(ch)[i] += s; }
    }
}

// ── Virtual defaults ──────────────────────────────────────────────────────────

void MetronomePlayerImpl::trigger_accent() {
    const auto sr = static_cast<float>(m_sample_rate);
    const float gain = get_parameter_float(Gain) * k_accent_gain_factor;
    pick_voice().trigger(sr, k_accent_freq, gain, k_accent_decay_ms);
}

void MetronomePlayerImpl::trigger_click() {
    const auto sr = static_cast<float>(m_sample_rate);
    const float gain = get_parameter_float(Gain) * k_click_gain_factor;
    pick_voice().trigger(sr, k_click_freq, gain, k_click_decay_ms);
}

float MetronomePlayerImpl::tick_voices() {
    const auto sr = static_cast<float>(m_sample_rate);
    float s = 0.0f;
    for (auto& v : m_voices) { s += v.tick(sr); }
    return s;
}

// ── Private helpers ───────────────────────────────────────────────────────────

MetronomePlayerImpl::Voice& MetronomePlayerImpl::pick_voice() {
    Voice* quietest = &m_voices[0];
    for (auto& v : m_voices) {
        if (v.m_env.get_current_level() < quietest->m_env.get_current_level()) { quietest = &v; }
    }
    return *quietest;
}

std::optional<uint32_t> MetronomePlayerImpl::crossing_offset(double division_beats,
                                                             double beat_start,
                                                             double bps,
                                                             uint32_t frame_count) const {
    const double next = std::ceil(beat_start / division_beats) * division_beats;
    const double beat_end = beat_start + static_cast<double>(frame_count) * bps;
    if (next >= beat_end) { return std::nullopt; }
    const auto offset = static_cast<uint32_t>((next - beat_start) / bps);
    // Float math could nudge offset to frame_count even after the next < beat_end check;
    // reject rather than clamp so the boundary fires cleanly at offset 0 of the next block.
    if (offset >= frame_count) { return std::nullopt; }
    return offset;
}

}  // namespace thl::dsp::metronome
