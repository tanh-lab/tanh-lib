#include <tanh/core/Numbers.h>
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

namespace thl::dsp::granular {

SamplePlayer::SamplePlayer(const SampleReader& reader, GrainVisualizer& viz)
    : m_reader(reader), m_viz(viz) {}

void SamplePlayer::prepare(double sample_rate, size_t num_channels) {
    m_sample_rate = sample_rate;
    // The per-frame scratch arrays are k_max_channel_support wide.
    m_channels = std::min(num_channels, k_max_channel_support);
    auto const sample_rate_f = static_cast<float>(sample_rate);
    m_fade_length = std::max(static_cast<size_t>(1),
                             static_cast<size_t>(k_player_crossfade_duration * sample_rate_f));
    m_min_span = std::max(static_cast<size_t>(1),
                          static_cast<size_t>(k_player_min_loop_duration * sample_rate_f));
    m_snap_radius = static_cast<size_t>(k_player_snap_radius * sample_rate_f);

    // Quarter-sine crossfade table, one entry per integer counter value the
    // fades can hold. Allocating here keeps the audio thread free of both the
    // allocation and the per-frame sin/cos.
    m_fade_curve.resize(m_fade_length + 1);
    for (size_t k = 0; k <= m_fade_length; ++k) {
        float const t = static_cast<float>(k) / static_cast<float>(m_fade_length);
        m_fade_curve[k] = std::sin(t * std::numbers::pi_v<float> * 0.5f);
    }

    m_marker_cache.m_valid = false;
    reset();
}

void SamplePlayer::reset() {
    if (m_started) { m_viz.head_finished(); }
    m_started = false;
    m_restart = false;
    m_finished = false;
    m_play_head = 0.0;
    m_fade_remaining = 0;
    m_fade_open = false;
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
    // A loop fade in progress holds the region it opened with: End and Loop
    // moving under it would change the blend mid-fade. The new markers land
    // at the wrap (a retrigger closes the fade, so it takes them at once).
    if (m_started && m_fade_open && !m_restart && params.m_loop) { src.m_region = m_region; }
    begin_or_switch(src);
    refresh_outgoing_banks();
    LoopPlan const plan = plan_loop(src);
    size_t const write_channels = std::min(m_channels, block.m_num_channels);

    channel_mixer::Frame frame{};
    for (size_t i = 0; i < block.m_num_frames; ++i) {
        if (m_finished) {
            // One-shot done: only the parked tail still sounds.
            frame.fill(0.0f);
        } else {
            update_loop_fade(src, params, plan);
            read_live_frame(src, params, frame);
        }
        mix_outgoing_tails(src, params, frame);
        for (size_t ch = 0; ch < write_channels; ++ch) { block.m_channels[ch][i] = frame[ch]; }
        if (!m_finished) { advance_head(src, params, plan); }
    }
    m_total_frames = src.m_frames;
    m_region = src.m_region;
    return true;
}

void SamplePlayer::report_visualization() const {
    if (!m_started || m_total_frames == 0) { return; }
    // A finished one-shot parks the head on End; keep the report inside the
    // region (physical() of a position past End would leave [0, 1]).
    double const head = std::min(m_play_head, static_cast<double>(m_region.m_end) - 1.0);
    m_viz.head_updated(static_cast<float>(m_region.physical(std::max(head, 0.0))) /
                       static_cast<float>(m_total_frames));
}

bool SamplePlayer::resolve_source(const VoiceParams& params, Source& out) {
    if (m_reader.num_banks() == 0) { return false; }
    out.m_bank = m_reader.clamp_bank(params.m_sample_index);
    // Only the selected semitones are rendered; an unselected slot is an
    // empty bank and the voice stays silent rather than reading it.
    out.m_frames = m_reader.num_frames(out.m_bank);
    out.m_channels = m_reader.num_channels(out.m_bank);
    if (out.m_frames == 0) { return false; }
    out.m_joined = false;
    out.m_region = params.m_slicer
                       ? SampleRegion::from_slices(params.m_sample_start,
                                                   params.m_sample_end,
                                                   params.m_slices,
                                                   out.m_frames)
                       : resolve_markers(params, out.m_bank, out.m_frames, out.m_joined);
    if (out.m_region.size() == 0) { return false; }
    // Velocity is the head's own rate here (varispeed). Modulation may push
    // it past the range; keep it forward and finite.
    out.m_velocity = std::clamp(params.m_velocity, k_min_varispeed, k_max_varispeed);
    return true;
}

SampleRegion SamplePlayer::resolve_markers(const VoiceParams& params,
                                           size_t bank,
                                           size_t total_frames,
                                           bool& joined) {
    Markers const raw{.m_start = SampleRegion::to_frame(params.m_sample_start, total_frames),
                      .m_end = SampleRegion::to_frame(params.m_sample_end, total_frames),
                      .m_loop = SampleRegion::to_frame(params.m_sample_loop_point, total_frames)};
    const float* const data = m_reader.banks()[bank].get_read_pointer(0);
    auto& cache = m_marker_cache;
    if (cache.m_valid && cache.m_raw == raw && cache.m_data == data &&
        cache.m_total == total_frames && cache.m_snap == params.m_loop_snap) {
        const auto& m = cache.m_resolved;
        joined = cache.m_joined;
        return SampleRegion::from_frames(m.m_start, m.m_end, m.m_loop, total_frames);
    }

    bool const snap = params.m_loop_snap;
    // The direction is the raw markers': a Start snapping past a close End
    // must not reverse the region.
    bool const forward = raw.m_end >= raw.m_start;
    Markers m = raw;
    if (snap) { m.m_start = snap_to_crossing(bank, m.m_start, 0, total_frames); }

    // Minimum span: End keeps its side of Start (End == Start plays
    // forwards) at least m_min_span away; at the sample's edge Start gives
    // way instead. Then End snaps, but never back inside the span.
    size_t const span = std::min(m_min_span, total_frames);
    if (forward) {
        if (m.m_end < m.m_start + span) {
            m.m_end = m.m_start + span;
            if (m.m_end > total_frames) {
                m.m_end = total_frames;
                m.m_start = total_frames - span;
            }
        }
        if (snap) { m.m_end = snap_to_crossing(bank, m.m_end, m.m_start + span, total_frames); }
    } else {
        if (m.m_end + span > m.m_start) {
            if (m.m_start < span) {
                m.m_start = span;
                m.m_end = 0;
            } else {
                m.m_end = m.m_start - span;
            }
        }
        if (snap) { m.m_end = snap_to_crossing(bank, m.m_end, 0, m.m_start - span); }
    }
    if (snap) { m.m_loop = snap_to_crossing(bank, m.m_loop, 0, total_frames); }
    auto const region = SampleRegion::from_frames(m.m_start, m.m_end, m.m_loop, total_frames);
    // In phase only if both ends of the jump found a crossing: End, and the
    // marker the head re-enters at — Start when the region loops whole
    // (Loop outside it or parked on End, see SampleRegion::virtual_loop).
    size_t const reentry =
        region.m_loop_point == region.m_start && m.m_loop != m.m_start ? m.m_start : m.m_loop;
    joined = snap && upward_crossing_at(bank, m.m_end) && upward_crossing_at(bank, reentry);

    cache = {.m_raw = raw,
             .m_resolved = m,
             .m_data = data,
             .m_total = total_frames,
             .m_snap = snap,
             .m_joined = joined,
             .m_valid = true};
    return region;
}

size_t SamplePlayer::snap_to_crossing(size_t bank, size_t target, size_t lo, size_t hi) const {
    target = std::clamp(target, lo, hi);
    auto const crossing_at = [this, bank](size_t i) { return upward_crossing_at(bank, i); };
    // Nearest first, alternating sides: at most 2 * radius + 1 checks, and
    // only when a marker actually moved (resolve_markers caches).
    for (size_t d = 0; d <= m_snap_radius; ++d) {
        bool const up_in = target + d <= hi;
        bool const down_in = target >= lo + d;
        if (!up_in && !down_in) { break; }
        if (up_in && crossing_at(target + d)) { return target + d; }
        if (d > 0 && down_in && crossing_at(target - d)) { return target - d; }
    }
    return target;
}

bool SamplePlayer::upward_crossing_at(size_t bank, size_t frame) const {
    // An upward crossing of the first two channels' sum at `frame`: the
    // frame before is negative, this one is not. Both markers of a wrap
    // snapping to the same direction is what makes the join like to like.
    const auto& buffer = m_reader.banks()[bank];
    if (frame == 0 || frame >= buffer.get_num_samples()) { return false; }
    const float* const a = buffer.get_read_pointer(0);
    const float* const b = buffer.get_num_channels() > 1 ? buffer.get_read_pointer(1) : nullptr;
    float const before = b != nullptr ? a[frame - 1] + b[frame - 1] : a[frame - 1];
    float const at = b != nullptr ? a[frame] + b[frame] : a[frame];
    return before < 0.0f && at >= 0.0f;
}

void SamplePlayer::begin_or_switch(const Source& src) {
    if (!m_started) {
        m_started = true;
        m_restart = false;
        m_play_head = static_cast<double>(src.m_region.m_start);
        m_sample_index = src.m_bank;
        m_fade_remaining = 0;
        m_fade_open = false;
        auto const total_f = static_cast<float>(src.m_frames);
        m_viz.head_started(
            static_cast<float>(src.m_region.m_start) / total_f,
            static_cast<float>(src.m_region.size()) / total_f,
            src.m_velocity,
            static_cast<float>(src.m_region.size()) / static_cast<float>(m_sample_rate) * 1000.0f);
    } else {
        rebase_to_region(src.m_region);
    }
    if (src.m_bank != m_sample_index) {
        // Pitch-bank switch: the banks are equal-length and time-aligned, so
        // the head position stays valid — only the waveform is discontinuous.
        start_crossfade(m_sample_index, m_reader.num_channels(m_sample_index));
        m_sample_index = src.m_bank;
    }
    if (m_restart) {
        // Retrigger while sounding: fade the old position out, restart at
        // the region start underneath it. A finished one-shot has already
        // parked its tail; the live head just comes back.
        m_restart = false;
        if (m_finished) {
            // The live head was silent, so there is nothing to park — but
            // the ADSR may still be in its release, so it fades in rather
            // than stepping to level x sample.
            m_fade_remaining = m_fade_length;
        } else {
            start_crossfade(src.m_bank, src.m_channels);
        }
        m_finished = false;
        m_fade_open = false;
        m_play_head = static_cast<double>(src.m_region.m_start);
    }
}

void SamplePlayer::rebase_to_region(const SampleRegion& region) {
    // The heads live in the region's virtual coordinates, but only the
    // physical frame they read is audible. Forward, virtual is physical, so
    // moving Start / End never moves a head. Reversed, the mirror axis is
    // Start + End: re-express every head against the new region so its
    // physical frame stays put (physical() is its own inverse), exactly as a
    // forward head ignores marker moves. Without this a drag or modulation of
    // either marker steps the reversed read position once per block.
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

void SamplePlayer::refresh_outgoing_banks() {
    // A bank that was unloaded since its tail was parked reads silence.
    for (auto& o : m_outgoing) {
        if (o.m_remaining > 0) { o.m_source_channels = m_reader.num_channels(o.m_sample_index); }
    }
}

double SamplePlayer::effective_loop_point(const Source& src) const {
    // Enforce a minimum loop body so a Loop marker dragged onto End can't
    // shrink the loop fade to nothing; a region smaller than that loops
    // whole.
    auto const region_end = static_cast<double>(src.m_region.m_end);
    double const min_loop =
        std::min(static_cast<double>(src.m_region.size()), static_cast<double>(m_min_span));
    return std::min(static_cast<double>(src.m_region.m_loop_point), region_end - min_loop);
}

SamplePlayer::LoopPlan SamplePlayer::plan_loop(const Source& src) const {
    // Fade lengths in source frames: the 10 ms fade at the current rate, at
    // most half the loop body, and no longer than the audio the copy reads —
    // before Loop for the fade before End, past End for the fade after the
    // wrap. Reversed, virtual "before Loop" is the sample above the region's
    // top and "past End" the sample below its bottom. The side with more
    // room wins, before End on a tie; under a frame there is no loop fade
    // and a wrap parks a tail instead.
    LoopPlan plan;
    plan.m_point = effective_loop_point(src);
    auto const lo = static_cast<double>(src.m_region.m_start);
    auto const hi = static_cast<double>(src.m_region.m_end);
    auto const total = static_cast<double>(src.m_frames);
    double const cap =
        std::min(static_cast<double>(m_fade_length) * static_cast<double>(src.m_velocity),
                 0.5 * (hi - plan.m_point));
    double const before_loop =
        src.m_region.m_reverse ? plan.m_point + total - lo - hi : plan.m_point;
    double const past_end = src.m_region.m_reverse ? lo : total - hi;
    double const pre = std::min(cap, before_loop);
    double const post = std::min(cap, past_end);
    if (pre >= post) {
        plan.m_pre = pre >= 1.0 ? pre : 0.0;
    } else {
        plan.m_post = post >= 1.0 ? post : 0.0;
    }
    // In phase only if the Loop the head jumps to is the snapped one (the
    // body floor can move it).
    plan.m_in_phase =
        src.m_joined && plan.m_point == static_cast<double>(src.m_region.m_loop_point);
    return plan;
}

void SamplePlayer::update_loop_fade(const Source& src,
                                    const VoiceParams& params,
                                    const LoopPlan& plan) {
    if (!params.m_loop) {
        // Loop switched off mid-fade: park the blend as it sounds now.
        if (m_fade_open) {
            start_crossfade(m_sample_index, src.m_channels);
            m_fade_open = false;
        }
        return;
    }
    auto const end = static_cast<double>(src.m_region.m_end);
    if (m_fade_open && m_fade_after_wrap && m_play_head >= m_fade_to) {
        m_fade_open = false;  // the copy past End has faded out
        return;
    }
    if (!m_fade_open) {
        if (plan.m_pre <= 0.0 || m_play_head < end - plan.m_pre || m_play_head >= end) { return; }
        // Normally the head enters at the fade's start; after a marker move
        // it may land further in, and the fade runs from where it is.
        open_fade(false, end, end - plan.m_point, plan.m_in_phase);
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
        m_loop_fade.m_copy_gain = fade_curve_at(copy_t);
        m_loop_fade.m_head_gain = fade_curve_at(1.0 - copy_t);
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

void SamplePlayer::read_live_frame(const Source& src,
                                   const VoiceParams& params,
                                   channel_mixer::Frame& frame) {
    if (m_fade_open) {
        read_blend(src,
                   params,
                   src.m_bank,
                   src.m_channels,
                   m_play_head,
                   m_loop_fade.m_copy_offset,
                   m_loop_fade.m_head_gain,
                   m_loop_fade.m_copy_gain,
                   frame);
    } else {
        channel_mixer::read_head_frame(m_reader,
                                       src.m_region.physical(m_play_head),
                                       src.m_bank,
                                       src.m_channels,
                                       params.m_channel_mode,
                                       params.m_spread,
                                       m_channels,
                                       frame);
    }
    if (m_fade_remaining == 0) { return; }
    float const gain_in = fade_in_gain(m_fade_remaining);
    for (size_t ch = 0; ch < m_channels; ++ch) { frame[ch] *= gain_in; }
    --m_fade_remaining;
}

void SamplePlayer::read_blend(const Source& src,
                              const VoiceParams& params,
                              size_t bank,
                              size_t source_channels,
                              double head,
                              double copy_offset,
                              float head_gain,
                              float copy_gain,
                              channel_mixer::Frame& frame) const {
    channel_mixer::Frame copy{};
    channel_mixer::read_head_frame(m_reader,
                                   src.m_region.physical(head),
                                   bank,
                                   source_channels,
                                   params.m_channel_mode,
                                   params.m_spread,
                                   m_channels,
                                   frame);
    channel_mixer::read_head_frame(m_reader,
                                   src.m_region.physical(head - copy_offset),
                                   bank,
                                   source_channels,
                                   params.m_channel_mode,
                                   params.m_spread,
                                   m_channels,
                                   copy);
    for (size_t ch = 0; ch < m_channels; ++ch) {
        frame[ch] = frame[ch] * head_gain + copy[ch] * copy_gain;
    }
}

void SamplePlayer::mix_outgoing_tails(const Source& src,
                                      const VoiceParams& params,
                                      channel_mixer::Frame& frame) {
    channel_mixer::Frame tail{};
    for (auto& o : m_outgoing) {
        if (o.m_remaining == 0) { continue; }
        float const gain_out = o.m_gain * fade_out_gain(o.m_remaining);
        if (o.m_source_channels > 0) {
            if (o.m_copy_gain > 0.0f) {
                read_blend(src,
                           params,
                           o.m_sample_index,
                           o.m_source_channels,
                           o.m_head,
                           o.m_copy_offset,
                           o.m_head_gain,
                           o.m_copy_gain,
                           tail);
            } else {
                channel_mixer::read_head_frame(m_reader,
                                               src.m_region.physical(o.m_head),
                                               o.m_sample_index,
                                               o.m_source_channels,
                                               params.m_channel_mode,
                                               params.m_spread,
                                               m_channels,
                                               tail);
            }
            for (size_t ch = 0; ch < m_channels; ++ch) { frame[ch] += tail[ch] * gain_out; }
        }
        o.m_head += src.m_velocity;
        --o.m_remaining;
    }
}

void SamplePlayer::advance_head(const Source& src,
                                const VoiceParams& params,
                                const LoopPlan& plan) {
    double const previous = m_play_head;
    m_play_head += src.m_velocity;
    auto const region_end = static_cast<double>(src.m_region.m_end);
    if (m_play_head < region_end) { return; }
    if (!params.m_loop) {
        // One-shot: park the live head so it rides out the crossfade past
        // End instead of hard-cutting, then fall silent.
        start_crossfade(src.m_bank, src.m_channels);
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
    double const loop_size = region_end - plan.m_point;
    double const overshoot = std::fmod(m_play_head - region_end, loop_size);
    bool const faded_before = m_fade_open && !m_fade_after_wrap;
    bool const fade_after = !faded_before && previous < region_end && plan.m_post > 0.0;
    if (!faded_before && !fade_after) { start_crossfade(src.m_bank, src.m_channels); }
    m_fade_open = false;
    m_play_head = plan.m_point + overshoot;
    if (fade_after) { open_fade(true, m_play_head + plan.m_post, -loop_size, plan.m_in_phase); }
}

float SamplePlayer::fade_curve_at(double t) const {
    auto const index = static_cast<size_t>(
        std::lround(std::clamp(t, 0.0, 1.0) * static_cast<double>(m_fade_length)));
    return m_fade_curve[std::min(index, m_fade_length)];
}

float SamplePlayer::fade_in_gain(size_t remaining) const {
    if (remaining == 0) { return 1.0f; }
    // sin((1 - remaining/L) * pi/2) == m_fade_curve[L - remaining].
    return m_fade_curve[m_fade_length - std::min(remaining, m_fade_length)];
}

float SamplePlayer::fade_out_gain(size_t remaining) const {
    // cos((1 - remaining/L) * pi/2) == sin(remaining/L * pi/2) == curve[remaining].
    return m_fade_curve[std::min(remaining, m_fade_length)];
}

void SamplePlayer::start_crossfade(size_t old_sample_index, size_t old_source_channels) {
    // Park the live head as it sounds right now — including a fade-in still
    // in progress and a loop fade's blend — then restart the live head's
    // fade-in. A free slot is preferred; otherwise the tail closest to done
    // (quietest) is stolen.
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
    slot->m_sample_index = old_sample_index;
    slot->m_source_channels = old_source_channels;
    slot->m_gain = fade_in_gain(m_fade_remaining);
    slot->m_remaining = m_fade_length;
    m_fade_remaining = m_fade_length;
}

}  // namespace thl::dsp::granular
