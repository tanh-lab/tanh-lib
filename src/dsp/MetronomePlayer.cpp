#include <tanh/dsp/MetronomePlayer.h>
#include <tanh/dsp/audio/AudioBufferView.h>
#include <tanh/transport/TransportClock.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>

namespace thl::dsp {

// ── Voice ─────────────────────────────────────────────────────────────────────

void MetronomePlayer::Voice::trigger(float sample_rate,
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

float MetronomePlayer::Voice::tick(float sample_rate) {
    if (!m_env.is_active()) { return 0.0f; }

    if (m_env.get_state() == thl::dsp::utils::ADSR::State::SUSTAIN) { m_env.note_off(); }

    const float envelope = m_env.process();
    const float sample = std::sin(m_phase * 2.0f * std::numbers::pi_v<float>);
    m_phase += m_freq / sample_rate;
    if (m_phase >= 1.0f) { m_phase -= 1.0f; }
    return m_gain * envelope * sample;
}

// ── MetronomePlayer ───────────────────────────────────────────────────────────

MetronomePlayer::MetronomePlayer(thl::TransportClock& clock) : m_clock(clock) {}

void MetronomePlayer::prepare(const double& sample_rate,
                              const size_t& /*samples_per_block*/,
                              const size_t& /*num_channels*/) {
    m_sample_rate = sample_rate;
}

void MetronomePlayer::process(thl::dsp::audio::AudioBufferView buffer, uint32_t modulation_offset) {
    const auto frame_count = static_cast<uint32_t>(buffer.get_num_frames());
    const bool enabled = get_parameter_bool(Enabled, modulation_offset);

    if (!enabled || !m_clock.is_playing()) {
        // Still tick active voices so they fade out cleanly
        for (uint32_t i = 0; i < frame_count; ++i) {
            const float s = tick_voices();
            for (size_t ch = 0; ch < buffer.get_num_channels(); ++ch) {
                buffer.get_write_pointer(ch)[i] += s;
            }
        }
        return;
    }

    const double bps = m_clock.bpm() / (60.0 * m_sample_rate);
    const double beat_start = m_clock.beat_at_sample(0);
    const auto bar_size = static_cast<double>(m_clock.sig_num());

    const thl::Division rhythm = int_to_division(get_parameter_int(Rhythm, modulation_offset));
    const double div_size = [&] {
        switch (rhythm) {
            case thl::Division::Bar: return bar_size;
            case thl::Division::Half: return 2.0;
            case thl::Division::Beat: return 1.0;
            case thl::Division::Eighth: return 0.5;
            case thl::Division::Sixteenth: return 0.25;
        }
        return 1.0;
    }();

    const auto bar_off = crossing_offset(bar_size, beat_start, bps, frame_count);
    const auto div_off = (div_size != bar_size)
                             ? crossing_offset(div_size, beat_start, bps, frame_count)
                             : std::nullopt;

    for (uint32_t i = 0; i < frame_count; ++i) {
        if (bar_off && i == *bar_off) {
            trigger_accent();
        } else if (div_off && i == *div_off) {
            trigger_click();
        }

        const float s = tick_voices();
        for (size_t ch = 0; ch < buffer.get_num_channels(); ++ch) {
            buffer.get_write_pointer(ch)[i] += s;
        }
    }
}

// ── Virtual defaults ──────────────────────────────────────────────────────────

void MetronomePlayer::trigger_accent() {
    const auto sr = static_cast<float>(m_sample_rate);
    const float gain = get_parameter_float(Gain) * k_accent_gain_factor;
    pick_voice().trigger(sr, k_accent_freq, gain, k_accent_decay_ms);
}

void MetronomePlayer::trigger_click() {
    const auto sr = static_cast<float>(m_sample_rate);
    const float gain = get_parameter_float(Gain) * k_click_gain_factor;
    pick_voice().trigger(sr, k_click_freq, gain, k_click_decay_ms);
}

float MetronomePlayer::tick_voices() {
    const auto sr = static_cast<float>(m_sample_rate);
    float s = 0.0f;
    for (auto& v : m_voices) { s += v.tick(sr); }
    return s;
}

// ── Private helpers ───────────────────────────────────────────────────────────

MetronomePlayer::Voice& MetronomePlayer::pick_voice() {
    Voice* quietest = &m_voices[0];
    for (auto& v : m_voices) {
        if (v.m_env.get_current_level() < quietest->m_env.get_current_level()) { quietest = &v; }
    }
    return *quietest;
}

thl::Division MetronomePlayer::int_to_division(int value) {
    switch (value) {
        case 0: return thl::Division::Bar;
        case 1: return thl::Division::Half;
        case 2: return thl::Division::Beat;
        case 3: return thl::Division::Eighth;
        case 4: return thl::Division::Sixteenth;
        default: return thl::Division::Beat;
    }
}

std::optional<uint32_t> MetronomePlayer::crossing_offset(double division_beats,
                                                         double beat_start,
                                                         double bps,
                                                         uint32_t frame_count) const {
    const double next = std::ceil(beat_start / division_beats) * division_beats;
    const double beat_end = beat_start + static_cast<double>(frame_count) * bps;
    if (next >= beat_end) { return std::nullopt; }
    const auto offset = static_cast<uint32_t>((next - beat_start) / bps);
    return std::min(offset, frame_count - 1u);
}

}  // namespace thl::dsp
