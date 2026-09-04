#include <tanh/dsp/granular/ChannelMixer.h>
#include <tanh/dsp/granular/GrainVisualizer.h>
#include <tanh/dsp/granular/GranularTypes.h>
#include <tanh/dsp/granular/SamplePlayer.h>
#include <tanh/dsp/granular/SampleReader.h>
#include <tanh/dsp/granular/SampleRegion.h>
#include <tanh/dsp/granular/VoiceParams.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>

namespace thl::dsp::granular {

SamplePlayer::SamplePlayer(const SampleReader& reader, GrainVisualizer& viz)
    : m_reader(reader), m_viz(viz) {}

void SamplePlayer::prepare(double sample_rate, size_t num_channels) {
    m_sample_rate = sample_rate;
    // The per-frame scratch arrays are k_max_channel_support wide.
    m_channels = std::min(num_channels, k_max_channel_support);
    m_fade_length = std::max(
        static_cast<size_t>(1),
        static_cast<size_t>(k_player_crossfade_duration * static_cast<float>(sample_rate)));
    reset();
}

void SamplePlayer::reset() {
    if (m_started) { m_viz.head_finished(); }
    m_started = false;
    m_restart = false;
    m_play_head = 0.0;
    m_fade_remaining = 0;
    for (auto& o : m_outgoing) { o.m_remaining = 0; }
}

void SamplePlayer::note_on() {
    // The head restarts at the region start on note-on. If it is still
    // sounding the ADSR keeps its level (legato), so a cold restart would be
    // a full-level step: crossfade instead.
    if (m_started) {
        m_restart = true;
    } else {
        reset();
    }
}

bool SamplePlayer::render(const AudioBlock& block, const VoiceParams& params) {
    Source src;
    if (!resolve_source(params, src)) {
        // Every silent early-out resets the player: leaving m_started set
        // keeps the post-block viz painting a frozen head as if it were
        // sounding.
        reset();
        return false;
    }
    begin_or_switch(src);
    refresh_outgoing_banks();
    double const loop_point = effective_loop_point(src);
    size_t const write_channels = std::min(m_channels, block.m_num_channels);

    channel_mixer::Frame frame{};
    for (size_t i = 0; i < block.m_num_frames; ++i) {
        read_live_frame(src, params, frame);
        mix_outgoing_tails(src, params, frame);
        for (size_t ch = 0; ch < write_channels; ++ch) { block.m_channels[ch][i] = frame[ch]; }
        advance_head(src, loop_point);
    }
    m_total_frames = src.m_frames;
    return true;
}

void SamplePlayer::report_visualization() const {
    if (!m_started || m_total_frames == 0) { return; }
    m_viz.head_updated(static_cast<float>(m_play_head) / static_cast<float>(m_total_frames));
}

bool SamplePlayer::resolve_source(const VoiceParams& params, Source& out) const {
    if (m_reader.num_banks() == 0) { return false; }
    out.m_bank = m_reader.clamp_bank(params.m_sample_index);
    // Only the selected semitones are rendered; an unselected slot is an
    // empty bank and the voice stays silent rather than reading it.
    out.m_frames = m_reader.num_frames(out.m_bank);
    out.m_channels = m_reader.num_channels(out.m_bank);
    if (out.m_frames == 0) { return false; }
    out.m_region = SampleRegion::from_normalized(params.m_sample_start,
                                                 params.m_sample_end,
                                                 params.m_sample_loop_point,
                                                 out.m_frames);
    if (out.m_region.size() == 0) { return false; }
    // Velocity is the head's own rate here (varispeed). Modulation may push
    // it past the range; keep it forward and finite.
    out.m_velocity = std::clamp(params.m_velocity, 0.01f, 8.0f);
    return true;
}

void SamplePlayer::begin_or_switch(const Source& src) {
    if (!m_started) {
        m_started = true;
        m_restart = false;
        m_play_head = static_cast<double>(src.m_region.m_start);
        m_sample_index = src.m_bank;
        m_fade_remaining = 0;
        auto const total_f = static_cast<float>(src.m_frames);
        m_viz.head_started(
            static_cast<float>(src.m_region.m_start) / total_f,
            static_cast<float>(src.m_region.size()) / total_f,
            src.m_velocity,
            static_cast<float>(src.m_region.size()) / static_cast<float>(m_sample_rate) * 1000.0f);
    } else if (src.m_bank != m_sample_index) {
        // Pitch-bank switch: the banks are equal-length and time-aligned, so
        // the head position stays valid — only the waveform is discontinuous.
        start_crossfade(m_sample_index, m_reader.num_channels(m_sample_index));
        m_sample_index = src.m_bank;
    }
    if (m_restart) {
        // Retrigger while sounding: fade the old position out, restart at
        // the region start underneath it.
        m_restart = false;
        start_crossfade(src.m_bank, src.m_channels);
        m_play_head = static_cast<double>(src.m_region.m_start);
    }
}

void SamplePlayer::refresh_outgoing_banks() {
    // A bank that was unloaded since its tail was parked reads silence.
    for (auto& o : m_outgoing) {
        if (o.m_remaining > 0) { o.m_source_channels = m_reader.num_channels(o.m_sample_index); }
    }
}

double SamplePlayer::effective_loop_point(const Source& src) const {
    // Enforce a minimum loop body of two crossfades at the current rate so a
    // Loop marker dragged onto End can't degenerate into per-frame fade
    // restarts; a region smaller than that loops whole. Scaled by velocity:
    // the body is in source frames but the fade runs in output frames.
    auto const region_end = static_cast<double>(src.m_region.m_end);
    double const min_loop =
        std::min(static_cast<double>(src.m_region.size()),
                 2.0 * static_cast<double>(m_fade_length) * static_cast<double>(src.m_velocity));
    return std::min(static_cast<double>(src.m_region.m_loop_point), region_end - min_loop);
}

void SamplePlayer::read_live_frame(const Source& src,
                                   const VoiceParams& params,
                                   channel_mixer::Frame& frame) {
    channel_mixer::read_head_frame(m_reader,
                                   m_play_head,
                                   src.m_bank,
                                   src.m_channels,
                                   params.m_channel_mode,
                                   params.m_spread,
                                   m_channels,
                                   frame);
    if (m_fade_remaining == 0) { return; }
    float const gain_in = fade_in_gain();
    for (size_t ch = 0; ch < m_channels; ++ch) { frame[ch] *= gain_in; }
    --m_fade_remaining;
}

void SamplePlayer::mix_outgoing_tails(const Source& src,
                                      const VoiceParams& params,
                                      channel_mixer::Frame& frame) {
    channel_mixer::Frame tail{};
    for (auto& o : m_outgoing) {
        if (o.m_remaining == 0) { continue; }
        float const t =
            1.0f - static_cast<float>(o.m_remaining) / static_cast<float>(m_fade_length);
        float const gain_out = o.m_gain * std::cos(t * std::numbers::pi_v<float> * 0.5f);
        if (o.m_source_channels > 0) {
            channel_mixer::read_head_frame(m_reader,
                                           o.m_head,
                                           o.m_sample_index,
                                           o.m_source_channels,
                                           params.m_channel_mode,
                                           params.m_spread,
                                           m_channels,
                                           tail);
            for (size_t ch = 0; ch < m_channels; ++ch) { frame[ch] += tail[ch] * gain_out; }
        }
        o.m_head += src.m_velocity;
        --o.m_remaining;
    }
}

void SamplePlayer::advance_head(const Source& src, double loop_point) {
    m_play_head += src.m_velocity;
    auto const region_end = static_cast<double>(src.m_region.m_end);
    if (m_play_head < region_end) { return; }
    // Loop wrap: the outgoing head rides out the crossfade (clamped to the
    // sample's last frame by read_clamped) while the live one restarts at
    // Loop, carrying the fractional overshoot so the seam is phase-exact.
    // The overshoot is wrapped into the loop body (an End drag below the
    // head can exceed it).
    double const loop_size = region_end - loop_point;
    double const overshoot = std::fmod(m_play_head - region_end, loop_size);
    start_crossfade(src.m_bank, src.m_channels);
    m_play_head = loop_point + overshoot;
}

float SamplePlayer::fade_in_gain() const {
    if (m_fade_remaining == 0) { return 1.0f; }
    float const t = 1.0f - static_cast<float>(m_fade_remaining) / static_cast<float>(m_fade_length);
    return std::sin(t * std::numbers::pi_v<float> * 0.5f);
}

void SamplePlayer::start_crossfade(size_t old_sample_index, size_t old_source_channels) {
    // Park the live head as it sounds right now — including a fade-in still
    // in progress — then restart the live head's fade-in. A free slot is
    // preferred; otherwise the tail closest to done (quietest) is stolen.
    OutgoingHead* slot = nullptr;
    for (auto& o : m_outgoing) {
        if (o.m_remaining == 0) {
            slot = &o;
            break;
        }
    }
    if (slot == nullptr) {
        slot = &m_outgoing[0];
        for (auto& o : m_outgoing) {
            if (o.m_remaining < slot->m_remaining) { slot = &o; }
        }
    }
    slot->m_head = m_play_head;
    slot->m_sample_index = old_sample_index;
    slot->m_source_channels = old_source_channels;
    slot->m_gain = fade_in_gain();
    slot->m_remaining = m_fade_length;
    m_fade_remaining = m_fade_length;
}

}  // namespace thl::dsp::granular
