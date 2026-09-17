#include <tanh/dsp/sampler/LoopCrossfade.h>
#include <tanh/dsp/sampler/LoopRegion.h>
#include <tanh/dsp/sampler/SamplePlayer.h>
#include <tanh/dsp/sampler/SampleView.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <span>

namespace thl::dsp::sampler {

namespace {

// One frame of `view` at `position`: its own channels, a mono source
// duplicated into every channel, silence past a multichannel source's own.
void read_source_frame(const SampleView& view,
                       double position,
                       size_t num_channels,
                       std::array<float, k_max_channels>& frame) {
    frame.fill(0.0f);
    if (!view.valid()) { return; }
    size_t const own = std::min(view.m_num_channels, num_channels);
    for (size_t ch = 0; ch < own; ++ch) { frame[ch] = read_clamped(view, ch, position); }
    if (view.m_num_channels == 1) {
        for (size_t ch = 1; ch < num_channels; ++ch) { frame[ch] = frame[0]; }
    }
}

}  // namespace

void SamplePlayer::prepare(double sample_rate, const PlayerSettings& settings) {
    m_sample_rate = sample_rate;
    auto const sample_rate_f = static_cast<float>(sample_rate);
    m_min_speed = settings.m_min_speed;
    m_max_speed = std::max(settings.m_min_speed, settings.m_max_speed);
    m_curve.prepare(static_cast<size_t>(settings.m_crossfade_seconds * sample_rate_f));
    m_min_span = std::max(static_cast<size_t>(1),
                          static_cast<size_t>(settings.m_min_loop_seconds * sample_rate_f));
    m_markers.set_spans(m_min_span,
                        static_cast<size_t>(settings.m_snap_radius_seconds * sample_rate_f));
    reset();
}

void SamplePlayer::reset() {
    m_started = false;
    m_just_started = false;
    m_restart = false;
    m_finished = false;
    m_play_head = 0.0;
    m_fade_remaining = 0;
    m_fade_open = false;
    for (auto& o : m_outgoing) { o.m_remaining = 0; }
}

void SamplePlayer::note_on() {
    // The head restarts at the region start on note-on. If it is still
    // sounding the envelope keeps its level (legato), so a cold restart would
    // be a full-level step: crossfade instead.
    if (m_started) {
        m_restart = true;
    } else {
        reset();
    }
}

bool SamplePlayer::render(std::span<const SampleView> sources,
                          size_t source,
                          float* const* out,
                          size_t num_channels,
                          size_t num_frames) {
    m_just_started = false;
    Block blk;
    if (!resolve_block(sources, source, num_channels, blk)) {
        // Every silent early-out resets the player: a started head left
        // behind would be reported as sounding.
        reset();
        return false;
    }
    // A loop fade in progress holds the region it opened with: End and Loop
    // moving under it would change the blend mid-fade. The new markers land
    // at the wrap (a retrigger closes the fade, so it takes them at once).
    if (m_started && m_fade_open && !m_restart && m_loop) { blk.m_region = m_region; }
    begin_or_switch(blk);
    refresh_outgoing_sources(blk);
    LoopPlan const plan = plan_loop(blk);

    Frame frame{};
    for (size_t i = 0; i < num_frames; ++i) {
        if (m_finished) {
            // One-shot done: only the parked tail still sounds.
            frame.fill(0.0f);
        } else {
            update_loop_fade(blk, plan);
            read_live_frame(blk, frame);
        }
        mix_outgoing_tails(blk, frame);
        for (size_t ch = 0; ch < blk.m_channels; ++ch) { out[ch][i] = frame[ch]; }
        if (!m_finished) { advance_head(blk, plan); }
    }
    m_total_frames = blk.m_frames;
    m_block_speed = blk.m_speed;
    m_region = blk.m_region;
    return true;
}

float SamplePlayer::normalized_position() const {
    if (!m_started || m_total_frames == 0) { return 0.0f; }
    // A finished one-shot parks the head on End; keep the report inside the
    // region (physical() of a position past End would leave [0, 1]).
    double const head = std::min(m_play_head, static_cast<double>(m_region.m_end) - 1.0);
    return static_cast<float>(m_region.physical(std::max(head, 0.0))) /
           static_cast<float>(m_total_frames);
}

bool SamplePlayer::resolve_block(std::span<const SampleView> sources,
                                 size_t source,
                                 size_t num_channels,
                                 Block& out) {
    if (sources.empty()) { return false; }
    out.m_sources = sources;
    out.m_source = std::min(source, sources.size() - 1);
    const SampleView& view = sources[out.m_source];
    if (!view.valid()) { return false; }
    out.m_frames = view.m_num_frames;
    out.m_channels = std::min(num_channels, k_max_channels);
    out.m_joined = false;
    out.m_region =
        m_use_region ? m_explicit_region
                     : m_markers.resolve(view, m_start, m_end, m_loop_marker, m_snap, out.m_joined);
    if (m_use_region) {
        out.m_region.m_end = std::min(out.m_region.m_end, out.m_frames);
        out.m_region.m_start = std::min(out.m_region.m_start, out.m_region.m_end);
        out.m_region.m_loop_point =
            std::clamp(out.m_region.m_loop_point, out.m_region.m_start, out.m_region.m_end);
    }
    if (out.m_region.size() == 0) { return false; }
    // Modulation may push the rate past the range; keep it forward and finite.
    out.m_speed = std::isfinite(m_speed) ? std::clamp(m_speed, m_min_speed, m_max_speed) : 1.0f;
    return true;
}

void SamplePlayer::begin_or_switch(const Block& blk) {
    if (!m_started) {
        m_started = true;
        m_just_started = true;
        m_restart = false;
        m_play_head = static_cast<double>(blk.m_region.m_start);
        m_source = blk.m_source;
        m_fade_remaining = 0;
        m_fade_open = false;
    } else {
        rebase_to_region(blk.m_region);
    }
    if (blk.m_source != m_source) {
        // Source switch: sources are equal-length and time-aligned, so the
        // head position stays valid; only the waveform is discontinuous.
        bool const old_valid = m_source < blk.m_sources.size() && blk.m_sources[m_source].valid();
        start_crossfade(m_source, old_valid);
        m_source = blk.m_source;
    }
    if (m_restart) {
        // Retrigger while sounding: fade the old position out, restart at
        // the region start underneath it. A finished one-shot has already
        // parked its tail; the live head just comes back.
        m_restart = false;
        if (m_finished) {
            // The live head was silent, so there is nothing to park, but the
            // envelope may still be releasing: fade in rather than step.
            m_fade_remaining = m_curve.length();
        } else {
            start_crossfade(blk.m_source, true);
        }
        m_finished = false;
        m_fade_open = false;
        m_play_head = static_cast<double>(blk.m_region.m_start);
    }
}

void SamplePlayer::rebase_to_region(const LoopRegion& region) {
    // The heads live in the region's virtual coordinates, but only the
    // physical frame they read is audible. Forward, virtual is physical, so
    // moving Start / End never moves a head. Reversed, the mirror axis is
    // Start + End: re-express every head against the new region so its
    // physical frame stays put (physical() is its own inverse). Without this
    // a drag or modulation of either marker steps the reversed read position
    // once per block.
    if (region.m_start == m_region.m_start && region.m_end == m_region.m_end &&
        region.m_reverse == m_region.m_reverse) {
        return;
    }
    m_play_head = region.physical(m_region.physical(m_play_head));
    for (auto& o : m_outgoing) {
        if (o.m_remaining > 0) { o.m_head = region.physical(m_region.physical(o.m_head)); }
    }
    m_region = region;
}

void SamplePlayer::refresh_outgoing_sources(const Block& blk) {
    // A source that was emptied since its tail was parked reads silence.
    for (auto& o : m_outgoing) {
        if (o.m_remaining > 0) {
            o.m_source_valid =
                o.m_source < blk.m_sources.size() && blk.m_sources[o.m_source].valid();
        }
    }
}

SamplePlayer::LoopPlan SamplePlayer::plan_loop(const Block& blk) const {
    LoopPlan plan;
    plan.m_fade =
        plan_loop_fade(blk.m_region,
                       blk.m_frames,
                       static_cast<double>(m_curve.length()) * static_cast<double>(blk.m_speed),
                       static_cast<double>(m_min_span));
    // In phase only if the Loop the head jumps to is the snapped one (the
    // body floor can move it).
    plan.m_in_phase =
        blk.m_joined && plan.m_fade.m_point == static_cast<double>(blk.m_region.m_loop_point);
    return plan;
}

void SamplePlayer::update_loop_fade(const Block& blk, const LoopPlan& plan) {
    if (!m_loop) {
        // Loop switched off mid-fade: park the blend as it sounds now.
        if (m_fade_open) {
            start_crossfade(m_source, true);
            m_fade_open = false;
        }
        return;
    }
    auto const end = static_cast<double>(blk.m_region.m_end);
    if (m_fade_open && m_fade_after_wrap && m_play_head >= m_fade_to) {
        m_fade_open = false;  // the copy past End has faded out
        return;
    }
    if (!m_fade_open) {
        double const pre = plan.m_fade.m_pre;
        if (pre <= 0.0 || m_play_head < end - pre || m_play_head >= end) { return; }
        // Normally the head enters at the fade's start; after a marker move
        // it may land further in, and the fade runs from where it is.
        open_fade(false, end, end - plan.m_fade.m_point, plan.m_in_phase);
    }
    double const span = m_fade_to - m_fade_from;
    double const t = span > 0.0 ? std::clamp((m_play_head - m_fade_from) / span, 0.0, 1.0) : 1.0;
    // Before End the copy (just before Loop) fades in; after the wrap the
    // copy (carrying on past End) fades out.
    double const copy_t = m_fade_after_wrap ? 1.0 - t : t;
    if (m_fade_in_phase) {
        // In phase, equal-power would bulge +3 dB mid-fade (a tremolo at the
        // loop rate on a short loop), so the gains sum to one instead.
        m_loop_fade.m_copy_gain = static_cast<float>(copy_t);
        m_loop_fade.m_head_gain = static_cast<float>(1.0 - copy_t);
    } else {
        m_loop_fade.m_copy_gain = m_curve.at(copy_t);
        m_loop_fade.m_head_gain = m_curve.at(1.0 - copy_t);
    }
}

void SamplePlayer::open_fade(bool after_wrap, double to, double copy_offset, bool in_phase) {
    m_fade_open = true;
    m_fade_after_wrap = after_wrap;
    m_fade_in_phase = in_phase;
    m_fade_from = m_play_head;
    m_fade_to = to;
    m_loop_fade.m_copy_offset = copy_offset;
}

void SamplePlayer::read_live_frame(const Block& blk, Frame& frame) {
    const SampleView& view = blk.m_sources[blk.m_source];
    if (m_fade_open) {
        read_blend(blk,
                   view,
                   m_play_head,
                   m_loop_fade.m_copy_offset,
                   m_loop_fade.m_head_gain,
                   m_loop_fade.m_copy_gain,
                   frame);
    } else {
        read_source_frame(view, blk.m_region.physical(m_play_head), blk.m_channels, frame);
    }
    if (m_fade_remaining == 0) { return; }
    float const gain_in = m_curve.in_gain(m_fade_remaining);
    for (size_t ch = 0; ch < blk.m_channels; ++ch) { frame[ch] *= gain_in; }
    --m_fade_remaining;
}

void SamplePlayer::read_blend(const Block& blk,
                              const SampleView& view,
                              double head,
                              double copy_offset,
                              float head_gain,
                              float copy_gain,
                              Frame& frame) const {
    Frame copy{};
    read_source_frame(view, blk.m_region.physical(head), blk.m_channels, frame);
    read_source_frame(view, blk.m_region.physical(head - copy_offset), blk.m_channels, copy);
    for (size_t ch = 0; ch < blk.m_channels; ++ch) {
        frame[ch] = frame[ch] * head_gain + copy[ch] * copy_gain;
    }
}

void SamplePlayer::mix_outgoing_tails(const Block& blk, Frame& frame) {
    Frame tail{};
    for (auto& o : m_outgoing) {
        if (o.m_remaining == 0) { continue; }
        float const gain_out = o.m_gain * m_curve.out_gain(o.m_remaining);
        if (o.m_source_valid) {
            const SampleView& view = blk.m_sources[o.m_source];
            if (o.m_copy_gain > 0.0f) {
                read_blend(blk,
                           view,
                           o.m_head,
                           o.m_copy_offset,
                           o.m_head_gain,
                           o.m_copy_gain,
                           tail);
            } else {
                read_source_frame(view, blk.m_region.physical(o.m_head), blk.m_channels, tail);
            }
            for (size_t ch = 0; ch < blk.m_channels; ++ch) { frame[ch] += tail[ch] * gain_out; }
        }
        o.m_head += blk.m_speed;
        --o.m_remaining;
    }
}

void SamplePlayer::advance_head(const Block& blk, const LoopPlan& plan) {
    double const previous = m_play_head;
    m_play_head += blk.m_speed;
    auto const region_end = static_cast<double>(blk.m_region.m_end);
    if (m_play_head < region_end) { return; }
    if (!m_loop) {
        // One-shot: park the live head so it rides out the crossfade past
        // End instead of hard-cutting, then fall silent.
        start_crossfade(blk.m_source, true);
        m_finished = true;
        return;
    }
    // Loop wrap, carrying the fractional overshoot so the seam is
    // phase-exact. After the fade before End the copy was already reading
    // what follows Loop, so the jump is silent. With the fade after the wrap
    // planned, it opens now: a copy carries on past End and fades out over
    // the head. With neither (End moved behind the head, or no room to fade)
    // the old position rides out a tail crossfade. The overshoot is wrapped
    // into the loop body (an End drag below the head can exceed it).
    double const point = plan.m_fade.m_point;
    double const loop_size = region_end - point;
    double const overshoot = std::fmod(m_play_head - region_end, loop_size);
    bool const faded_before = m_fade_open && !m_fade_after_wrap;
    bool const fade_after = !faded_before && previous < region_end && plan.m_fade.m_post > 0.0;
    if (!faded_before && !fade_after) { start_crossfade(blk.m_source, true); }
    m_fade_open = false;
    m_play_head = point + overshoot;
    if (fade_after) {
        open_fade(true, m_play_head + plan.m_fade.m_post, -loop_size, plan.m_in_phase);
    }
}

void SamplePlayer::start_crossfade(size_t old_source, bool old_source_valid) {
    // Park the live head as it sounds right now (including a fade-in still in
    // progress and a loop fade's blend), then restart the live head's fade-in.
    // A free slot is preferred; otherwise the tail closest to done (quietest)
    // is stolen.
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
    if (m_fade_open) {
        slot->m_copy_offset = m_loop_fade.m_copy_offset;
        slot->m_head_gain = m_loop_fade.m_head_gain;
        slot->m_copy_gain = m_loop_fade.m_copy_gain;
    } else {
        slot->m_copy_offset = 0.0;
        slot->m_head_gain = 1.0f;
        slot->m_copy_gain = 0.0f;
    }
    slot->m_source = old_source;
    slot->m_source_valid = old_source_valid;
    slot->m_gain = m_curve.in_gain(m_fade_remaining);
    slot->m_remaining = m_curve.length();
    m_fade_remaining = m_curve.length();
}

}  // namespace thl::dsp::sampler
