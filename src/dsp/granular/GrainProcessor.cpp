#include <tanh/core/Numbers.h>
#include <tanh/dsp/granular/GrainProcessor.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>

#include "tanh/core/BufferView.h"
#include "tanh/dsp/audio/AudioDataStore.h"
#include "tanh/dsp/granular/GrainVisualizationListener.h"

namespace thl::dsp::granular {

GrainProcessorImpl::GrainProcessorImpl(audio::AudioDataStore& audio_store)
    : m_audio_store(audio_store)
    , m_random_generator(std::random_device{}())
    , m_uni_dist(0.0f, 1.0f) {
    // Prepare grain container
    m_grains.resize(m_max_grains);

    // Initialize all grains as inactive
    for (auto& grain : m_grains) { grain.m_active = false; }
}

GrainProcessorImpl::~GrainProcessorImpl() = default;

void GrainProcessorImpl::set_visualization_listener(GrainVisualizationListener* listener) {
    m_viz_listeners.clear();
    if (listener) { m_viz_listeners.push_back(listener); }
}

void GrainProcessorImpl::add_visualization_listener(GrainVisualizationListener* listener) {
    if (listener) { m_viz_listeners.push_back(listener); }
}

void GrainProcessorImpl::remove_visualization_listener(GrainVisualizationListener* listener) {
    std::erase(m_viz_listeners, listener);
}

void GrainProcessorImpl::set_visualization_update_rate(float fps) {
    if (fps > 0.f && m_sample_rate > 0) {
        m_viz_update_interval = static_cast<size_t>(m_sample_rate / fps);
    } else {
        m_viz_update_interval = 0;
    }
}

void GrainProcessorImpl::reset_grains() {
    for (auto& grain : m_grains) { grain.m_active = false; }
    m_sequential_position = 0;
    m_next_grain_time = 0;
    m_last_playing_state = false;
    m_playback_elapsed_samples = 0;
    m_envelope.reset();
    reset_player();
    m_mode_fade_out = false;
    m_mode_gain = 1.0f;
}

void GrainProcessorImpl::reset_player() {
    if (m_player_started) {
        for (auto* l : m_viz_listeners) { l->on_grain_finished(0); }
    }
    m_player_started = false;
    m_play_head = 0.0;
    m_fade_head = 0.0;
    m_fade_remaining = 0;
}

void GrainProcessorImpl::prepare(const double& sample_rate,
                                 const size_t& /*samples_per_block*/,
                                 const size_t& num_channels) {
    m_sample_rate = sample_rate;
    m_channels = num_channels;

    for (auto& grain : m_grains) {
        grain.m_envelope.set_sample_rate(static_cast<float>(m_sample_rate));
    }
    m_next_grain_time = 0;

    // Mode / player timing. The active mode seeds from the parameter so a
    // preset that boots in Sample mode doesn't run one block of grains first.
    m_mode_gain_step =
        1.0f / std::max(1.0f, k_mode_change_fade_duration * static_cast<float>(m_sample_rate));
    m_fade_length = std::max(
        static_cast<size_t>(1),
        static_cast<size_t>(k_player_crossfade_duration * static_cast<float>(m_sample_rate)));
    m_active_mode =
        static_cast<EngineMode>(std::clamp(get_parameter<int>(EngineModeParam),
                                           0,
                                           static_cast<int>(EngineMode::NumEngineModes) - 1));
    m_mode_gain = 1.0f;
    m_mode_fade_out = false;
    reset_player();

    m_envelope.set_sample_rate(static_cast<float>(m_sample_rate));
    m_envelope.set_parameters(get_parameter<float>(EnvelopeAttack),
                              get_parameter<float>(EnvelopeDecay),
                              get_parameter<float>(EnvelopeSustain),
                              get_parameter<float>(EnvelopeRelease),
                              get_parameter<float>(EnvelopeAttackCurve),
                              get_parameter<float>(EnvelopeDecayCurve),
                              get_parameter<float>(EnvelopeReleaseCurve));
    m_envelope.reset();
}

void GrainProcessorImpl::process(thl::core::BufferView buffer, uint32_t modulation_offset) {
    const size_t num_samples = buffer.get_num_samples();
    const size_t num_channels =
        std::min(buffer.get_num_channels(), static_cast<size_t>(k_max_channel_support));
    std::array<float*, k_max_channel_support> channel_ptrs{};
    for (size_t ch = 0; ch < num_channels; ++ch) {
        channel_ptrs[ch] = buffer.get_write_pointer(ch);
    }
    m_block_channels = num_channels;

    update_envelope_if_needed(modulation_offset);
    update_mode_fade(modulation_offset);

    bool const playing = get_parameter<bool>(Playing, modulation_offset);

    bool const envelope_active = m_envelope.is_active();
    if (playing && !envelope_active || playing && !m_last_playing_state) {
        m_envelope.note_on();
        m_next_grain_time = 0;      // Reset grain time when starting playback
        m_sequential_position = 0;  // Reset sequential position when starting
                                    // playback
        m_playback_elapsed_samples = 0;
        reset_player();  // Sample head restarts at region start on note-on
    } else if (!playing && m_envelope.get_state() != utils::ADSR::State::IDLE &&
               m_envelope.get_state() != utils::ADSR::State::RELEASE) {
        m_envelope.note_off();
    }
    m_last_playing_state = playing;

    float const volume = get_parameter<float>(Volume, modulation_offset);

    // Clear the buffer
    for (size_t ch = 0; ch < num_channels; ++ch) {
        std::memset(channel_ptrs[ch], 0, num_samples * sizeof(float));
    }

    // If not playing or no audio data, just return (silence)
    if (!m_envelope.is_active() || !m_audio_store.is_loaded()) {
        // Deactivate any lingering grains and notify visualization
        if (!m_viz_listeners.empty()) {
            for (size_t gi = 0; gi < m_grains.size(); ++gi) {
                if (m_grains[gi].m_active) {
                    m_grains[gi].m_active = false;
                    for (auto* l : m_viz_listeners) { l->on_grain_finished(static_cast<int>(gi)); }
                }
            }
            for (auto* l : m_viz_listeners) { l->on_master_envelope_updated(0.f); }
        }
        reset_player();
        return;
    }

    // Head policy: one continuous head (Sample) or the grain scheduler
    // (Position / Loop — they differ only inside calculate_start_position).
    if (m_active_mode == EngineMode::Sample) {
        render_player(channel_ptrs.data(), num_samples, modulation_offset);
    } else {
        update_grains(channel_ptrs.data(), num_samples, modulation_offset);
    }

    // Apply master volume, ADSR and the mode-change fade. The fade ramps
    // linearly toward 0 while a mode switch is pending and back to 1 after
    // the switch has happened (see update_mode_fade).
    float const mode_target = m_mode_fade_out ? 0.0f : 1.0f;
    for (size_t i = 0; i < num_samples; i++) {
        if (m_mode_gain < mode_target) {
            m_mode_gain = std::min(mode_target, m_mode_gain + m_mode_gain_step);
        } else if (m_mode_gain > mode_target) {
            m_mode_gain = std::max(mode_target, m_mode_gain - m_mode_gain_step);
        }
        float const grain_volume = volume * m_envelope.process() * m_mode_gain;
        for (size_t ch = 0; ch < num_channels; ++ch) { channel_ptrs[ch][i] *= grain_volume; }
    }

    // Player visualization — per block, NOT rate-limited like the grain path
    // (48 grains per voice earn a limit; one head doesn't), and AFTER the
    // envelope loop so the reported level is this block's, not the previous
    // one's. Both halves of the old scheme painted a fade-in the ADSR never
    // produced: on_grain_triggered seeds the slot's envelope to 0 (right for
    // a Hann grain, wrong for an instantly-loud head), and at note-on the
    // pre-block master level still read 0.
    if (m_active_mode == EngineMode::Sample && m_player_started && !m_viz_listeners.empty() &&
        m_player_total_frames > 0) {
        float const master_env = m_envelope.get_current_level();
        float const norm_pos =
            static_cast<float>(m_play_head) / static_cast<float>(m_player_total_frames);
        for (auto* l : m_viz_listeners) {
            l->on_master_envelope_updated(master_env);
            l->on_grain_updated(0, norm_pos, 1.0f);
        }
    }
}

void GrainProcessorImpl::update_mode_fade(uint32_t modulation_offset) {
    auto const requested =
        static_cast<EngineMode>(std::clamp(get_parameter<int>(EngineModeParam, modulation_offset),
                                           0,
                                           static_cast<int>(EngineMode::NumEngineModes) - 1));
    if (requested == m_active_mode) {
        m_mode_fade_out = false;
        return;
    }
    // A sounding voice fades to silence first; a silent one (or one whose
    // fade has landed) switches right away and ramps back up.
    if (m_envelope.is_active() && m_mode_gain > 0.0f) {
        m_mode_fade_out = true;
        return;
    }
    m_active_mode = requested;
    m_mode_fade_out = false;
    if (!m_envelope.is_active()) { m_mode_gain = 1.0f; }
    for (size_t gi = 0; gi < m_grains.size(); ++gi) {
        if (m_grains[gi].m_active) {
            m_grains[gi].m_active = false;
            for (auto* l : m_viz_listeners) { l->on_grain_finished(static_cast<int>(gi)); }
        }
    }
    m_sequential_position = 0;
    m_next_grain_time = 0;
    reset_player();
}

// ── Sample mode ──────────────────────────────────────────────────────────────

void GrainProcessorImpl::start_player_crossfade(size_t old_sample_index) {
    m_fade_head = m_play_head;
    m_fade_sample_index = old_sample_index;
    m_fade_remaining = m_fade_length;
}

void GrainProcessorImpl::read_player_frame(double position,
                                           size_t sample_index,
                                           size_t source_channels,
                                           ChannelMode mode,
                                           float width,
                                           std::array<float, k_max_channel_support>& out) {
    out.fill(0.0f);
    switch (mode) {
        case ChannelMode::MonoToStereo: {
            // One head has no per-grain pan to spread, so the mono sum sits
            // centred; Spread has nothing to widen here.
            float mono = 0.0f;
            for (size_t ch = 0; ch < source_channels; ++ch) {
                float s = 0.0f;
                read_sample_exact(position, sample_index, ch, s);
                mono += s;
            }
            if (source_channels > 1) { mono /= static_cast<float>(source_channels); }
            out[0] = mono;
            out[1] = mono;
            break;
        }
        case ChannelMode::TrueStereo: {
            float s0 = 0.0f, s1 = 0.0f;
            read_sample_exact(position, sample_index, 0, s0);
            if (source_channels > 1) {
                read_sample_exact(position, sample_index, 1, s1);
            } else {
                s1 = s0;
            }
            // Spread as mid/side width: 0 collapses to mono, 1 keeps the
            // source's own image.
            float const mid = 0.5f * (s0 + s1);
            float const side = 0.5f * (s0 - s1) * width;
            out[0] = mid + side;
            out[1] = mid - side;
            break;
        }
        case ChannelMode::TrueMultichannel: {
            size_t const out_channels = std::min(m_channels, source_channels);
            for (size_t ch = 0; ch < out_channels; ++ch) {
                read_sample_exact(position, sample_index, ch, out[ch]);
            }
            // Width per L/R pair (0,1), (2,3), ...
            for (size_t ch = 0; ch + 1 < out_channels; ch += 2) {
                float const mid = 0.5f * (out[ch] + out[ch + 1]);
                float const side = 0.5f * (out[ch] - out[ch + 1]) * width;
                out[ch] = mid + side;
                out[ch + 1] = mid - side;
            }
            break;
        }
        default: break;
    }
}

void GrainProcessorImpl::render_player(float** buffer,
                                       size_t n_buffer_frames,
                                       uint32_t modulation_offset) {
    // Every silent early-out resets the player: leaving m_player_started set
    // keeps the post-block viz painting a frozen head as if it were sounding.
    const auto& audio_data = m_audio_store.get_buffer();
    if (audio_data.empty()) {
        reset_player();
        return;
    }

    size_t const sample_index =
        static_cast<size_t>(std::clamp(get_parameter<int>(SampleIndex, modulation_offset),
                                       0,
                                       static_cast<int>(audio_data.size()) - 1));
    // Only the selected semitones are rendered; an unselected slot is an
    // empty buffer and the voice stays silent rather than reading it.
    if (audio_data[sample_index].empty()) {
        reset_player();
        return;
    }
    const auto& buf = audio_data[sample_index];
    size_t const total_frames = buf.get_num_samples();
    size_t const source_channels = buf.get_num_channels();
    if (total_frames == 0) {
        reset_player();
        return;
    }

    auto const region = compute_sample_region(total_frames, modulation_offset);
    if (region.size() == 0) {
        reset_player();
        return;
    }

    // Velocity is the head's own rate here (varispeed). Modulation may push
    // it past the range; keep it forward and finite.
    float const velocity =
        std::clamp(get_parameter<float>(Velocity, modulation_offset), 0.01f, 8.0f);
    auto const mode =
        static_cast<ChannelMode>(std::clamp(get_parameter<int>(ChannelModeParam, modulation_offset),
                                            0,
                                            static_cast<int>(ChannelMode::NumChannelModes) - 1));
    float const width = std::clamp(get_parameter<float>(Spread, modulation_offset), 0.0f, 1.0f);

    auto const total_f = static_cast<float>(total_frames);
    if (!m_player_started) {
        m_player_started = true;
        m_play_head = static_cast<double>(region.m_start);
        m_player_sample_index = sample_index;
        m_fade_remaining = 0;
        // The head occupies grain slot 0 in the visualisation.
        for (auto* l : m_viz_listeners) {
            l->on_grain_triggered(
                0,
                static_cast<float>(region.m_start) / total_f,
                static_cast<float>(region.size()) / total_f,
                velocity,
                static_cast<float>(region.size()) / static_cast<float>(m_sample_rate) * 1000.0f);
        }
    } else if (sample_index != m_player_sample_index) {
        // Pitch-bank switch: the banks are equal-length and time-aligned, so
        // the head position stays valid — only the waveform is discontinuous.
        start_player_crossfade(m_player_sample_index);
        m_player_sample_index = sample_index;
    }

    size_t fade_channels = 0;
    if (m_fade_remaining > 0 && m_fade_sample_index < audio_data.size() &&
        !audio_data[m_fade_sample_index].empty()) {
        fade_channels = audio_data[m_fade_sample_index].get_num_channels();
    }

    auto const region_end = static_cast<double>(region.m_end);
    // Enforce a minimum loop body (two crossfades, ~20 ms) so a Loop marker
    // dragged onto End can't degenerate into per-frame fade restarts and a
    // frozen output; a region smaller than that loops whole.
    double const min_loop =
        std::min(static_cast<double>(region.size()), 2.0 * static_cast<double>(m_fade_length));
    double const loop_point =
        std::min(static_cast<double>(region.m_loop_point), region_end - min_loop);
    std::array<float, k_max_channel_support> frame{};
    std::array<float, k_max_channel_support> fade_frame{};

    for (size_t i = 0; i < n_buffer_frames; ++i) {
        read_player_frame(m_play_head, sample_index, source_channels, mode, width, frame);

        if (m_fade_remaining > 0) {
            if (fade_channels > 0) {
                read_player_frame(m_fade_head,
                                  m_fade_sample_index,
                                  fade_channels,
                                  mode,
                                  width,
                                  fade_frame);
            } else {
                fade_frame.fill(0.0f);
            }
            float const t =
                1.0f - static_cast<float>(m_fade_remaining) / static_cast<float>(m_fade_length);
            float const gain_in = std::sin(t * std::numbers::pi_v<float> * 0.5f);
            float const gain_out = std::cos(t * std::numbers::pi_v<float> * 0.5f);
            for (size_t ch = 0; ch < m_channels; ++ch) {
                frame[ch] = frame[ch] * gain_in + fade_frame[ch] * gain_out;
            }
            m_fade_head += velocity;
            --m_fade_remaining;
        }

        size_t const write_channels = std::min(m_channels, m_block_channels);
        for (size_t ch = 0; ch < write_channels; ++ch) { buffer[ch][i] = frame[ch]; }

        m_play_head += velocity;
        if (m_play_head >= region_end) {
            // Loop wrap: the outgoing head rides out the crossfade (clamped to
            // the sample's last frame by read_sample_exact) while the new one
            // restarts at Loop, carrying the fractional overshoot so the seam
            // is phase-exact. The overshoot is wrapped into the loop body (an
            // End drag below the head can exceed it), and a crossfade already
            // in flight is left to finish rather than pinned at t = 0.
            double const loop_size = region_end - loop_point;
            double const overshoot = std::fmod(m_play_head - region_end, loop_size);
            if (m_fade_remaining == 0) {
                start_player_crossfade(sample_index);
                fade_channels = source_channels;
            }
            m_play_head = loop_point + overshoot;
        }
        m_playback_elapsed_samples++;
    }

    m_player_total_frames = total_frames;
}

void GrainProcessorImpl::update_envelope_if_needed(uint32_t modulation_offset) {
    float const attack = get_parameter<float>(EnvelopeAttack, modulation_offset);
    float const decay = get_parameter<float>(EnvelopeDecay, modulation_offset);
    float const sustain = get_parameter<float>(EnvelopeSustain, modulation_offset);
    float const release = get_parameter<float>(EnvelopeRelease, modulation_offset);

    if (attack != m_last_envelope_attack) {
        m_envelope.set_attack(attack);
        m_last_envelope_attack = attack;
    }
    if (decay != m_last_envelope_decay) {
        m_envelope.set_decay(decay);
        m_last_envelope_decay = decay;
    }
    if (sustain != m_last_envelope_sustain) {
        m_envelope.set_sustain(sustain);
        m_last_envelope_sustain = sustain;
    }
    if (release != m_last_envelope_release) {
        m_envelope.set_release(release);
        m_last_envelope_release = release;
    }

    float const attack_curve = get_parameter<float>(EnvelopeAttackCurve, modulation_offset);
    float const decay_curve = get_parameter<float>(EnvelopeDecayCurve, modulation_offset);
    float const release_curve = get_parameter<float>(EnvelopeReleaseCurve, modulation_offset);

    if (attack_curve != m_last_envelope_attack_curve) {
        m_envelope.set_attack_curve(attack_curve);
        m_last_envelope_attack_curve = attack_curve;
    }
    if (decay_curve != m_last_envelope_decay_curve) {
        m_envelope.set_decay_curve(decay_curve);
        m_last_envelope_decay_curve = decay_curve;
    }
    if (release_curve != m_last_envelope_release_curve) {
        m_envelope.set_release_curve(release_curve);
        m_last_envelope_release_curve = release_curve;
    }
}

void GrainProcessorImpl::update_grains(float** buffer,
                                       size_t n_buffer_frames,
                                       uint32_t modulation_offset) {
    const auto& audio_data = m_audio_store.get_buffer();
    // Modulation may push values past the parameter range (SmartHandle does not
    // clamp plain-space offsets); clamp here so an LFO trough can't collapse the
    // trigger rate and silence a held voice.
    float const density = std::clamp(get_parameter<float>(Density, modulation_offset), 0.0f, 1.0f);

    // Calculate how frequently we should trigger new grains. Density maps
    // exponentially in rate so equal knob travel gives equal rate ratios.
    float const rate = k_min_grain_rate * std::pow(k_max_grain_rate / k_min_grain_rate, density);
    m_min_grain_interval = static_cast<unsigned int>(m_sample_rate / rate);

    size_t const sample_index =
        static_cast<size_t>(std::clamp(get_parameter<int>(SampleIndex, modulation_offset),
                                       0,
                                       static_cast<int>(audio_data.size()) - 1));

    if (m_current_sample_index != sample_index) {
        m_current_sample_index = sample_index;
        m_next_grain_time = 0;
    }

    // Determine source channel count for the current sample
    size_t source_channels = 1;
    if (sample_index < audio_data.size() && !audio_data[sample_index].empty()) {
        source_channels = audio_data[sample_index].get_num_channels();
    }

    auto mode =
        static_cast<ChannelMode>(std::clamp(get_parameter<int>(ChannelModeParam, modulation_offset),
                                            0,
                                            static_cast<int>(ChannelMode::NumChannelModes) - 1));
    float const spread = std::clamp(get_parameter<float>(Spread, modulation_offset), 0.0f, 1.0f);

    // Get total frames for visualization position normalization
    size_t total_frames = 0;
    if (sample_index < audio_data.size() && !audio_data[sample_index].empty()) {
        total_frames = audio_data[sample_index].get_num_samples();
    }

    // For each sample in the buffer
    for (unsigned int i = 0; i < n_buffer_frames; i++) {
        // Check if it's time to trigger a new grain
        if (m_next_grain_time <= 0) {
            trigger_grain(sample_index, modulation_offset);
            m_next_grain_time = m_min_grain_interval - 1;
        } else {
            m_next_grain_time--;
        }

        // Accumulate per-channel samples
        std::array<float, k_max_channel_support> channel_accum{};

        // Process all active grains
        for (size_t gi = 0; gi < m_grains.size(); ++gi) {
            auto& grain = m_grains[gi];
            if (!grain.m_active) { continue; }

            // Hann window envelope for amplitude control
            float const normalized_position = static_cast<float>(grain.m_current_position) /
                                              static_cast<float>(grain.m_grain_size);
            float const envelope = grain.m_envelope.process_at_position(normalized_position);

            // Check if the grain should be deactivated
            if (!grain.m_envelope.is_active() || normalized_position >= 1.0f) {
                grain.m_active = false;
                for (auto* l : m_viz_listeners) { l->on_grain_finished(static_cast<int>(gi)); }
                continue;
            }

            // Calculate the current position in the source audio
            float const source_pos =
                static_cast<float>(grain.m_start_position) +
                (static_cast<float>(grain.m_current_position) * grain.m_velocity);

            float const position_spread = 0.5f + (grain.m_position_spread - 0.5f) * spread;

            switch (mode) {
                case ChannelMode::MonoToStereo: {
                    // Mono sum of all source channels
                    float mono_sample = 0.0f;
                    for (size_t ch = 0; ch < source_channels; ++ch) {
                        float s = 0.0f;
                        read_sample(source_pos, grain.m_sample_index, ch, s);
                        mono_sample += s;
                    }
                    if (source_channels > 1) { mono_sample /= static_cast<float>(source_channels); }
                    mono_sample *= envelope;
                    channel_accum[0] += mono_sample * (1.0f - position_spread);
                    channel_accum[1] += mono_sample * position_spread;
                    break;
                }
                case ChannelMode::TrueStereo: {
                    float s0 = 0.0f, s1 = 0.0f;
                    read_sample(source_pos, grain.m_sample_index, 0, s0);
                    if (source_channels > 1) {
                        read_sample(source_pos, grain.m_sample_index, 1, s1);
                    } else {
                        s1 = s0;
                    }
                    // Spread redistributes per-grain energy across L/R
                    channel_accum[0] += s0 * envelope * (1.0f - position_spread) * 2.0f;
                    channel_accum[1] += s1 * envelope * position_spread * 2.0f;
                    break;
                }
                case ChannelMode::TrueMultichannel: {
                    size_t const out_channels = std::min(m_channels, source_channels);
                    for (size_t ch = 0; ch < out_channels; ++ch) {
                        float s = 0.0f;
                        read_sample(source_pos, grain.m_sample_index, ch, s);
                        // Even channels (0,2,...) get left energy, odd channels
                        // (1,3,...) get right energy
                        float const energy = (ch % 2 == 0) ? (1.0f - position_spread) * 2.0f
                                                           : position_spread * 2.0f;
                        channel_accum[ch] += s * envelope * energy;
                    }
                    break;
                }
                default: break;
            }

            // Update grain position
            grain.m_current_position++;

            // Deactivate if grain is finished
            if (grain.m_current_position >= grain.m_grain_size) {
                grain.m_active = false;
                for (auto* l : m_viz_listeners) { l->on_grain_finished(static_cast<int>(gi)); }
            }
        }

        // Write to output buffer (planar layout). Clamped to the channels the
        // block actually carries — a device switch can deliver fewer than
        // prepare() promised, and the pointer array holds nullptr beyond them.
        for (size_t ch = 0; ch < std::min(m_channels, m_block_channels); ++ch) {
            buffer[ch][i] = channel_accum[ch];
        }

        m_playback_elapsed_samples++;
    }

    // Rate-limited visualization update
    if (!m_viz_listeners.empty() && m_viz_update_interval > 0 && total_frames > 0) {
        m_viz_update_counter += n_buffer_frames;
        if (m_viz_update_counter >= m_viz_update_interval) {
            m_viz_update_counter = 0;
            auto total_f = static_cast<float>(total_frames);

            // Send current master envelope value
            float const master_env = m_envelope.get_current_level();
            for (auto* l : m_viz_listeners) { l->on_master_envelope_updated(master_env); }

            for (size_t gi = 0; gi < m_grains.size(); ++gi) {
                auto& grain = m_grains[gi];
                if (!grain.m_active) { continue; }
                float const current_pos =
                    (static_cast<float>(grain.m_start_position) +
                     static_cast<float>(grain.m_current_position) * grain.m_velocity) /
                    total_f;
                float const normalized_position = static_cast<float>(grain.m_current_position) /
                                                  static_cast<float>(grain.m_grain_size);
                float const envelope = grain.m_envelope.process_at_position(normalized_position);
                for (auto* l : m_viz_listeners) {
                    l->on_grain_updated(static_cast<int>(gi), current_pos, envelope);
                }
            }
        }
    }
}

void GrainProcessorImpl::trigger_grain(const size_t sample_index, uint32_t modulation_offset) {
    const auto& audio_data = m_audio_store.get_buffer();

    // Find an inactive grain slot
    for (size_t gi = 0; gi < m_grains.size(); ++gi) {
        auto& grain = m_grains[gi];
        if (!grain.m_active) {
            if (sample_index >= audio_data.size() || audio_data[sample_index].empty()) { return; }

            // Get parameters needed for grain_size setup
            float const grain_size_param = get_parameter<float>(Size, modulation_offset);
            float const size_temperature = get_parameter<float>(TemperatureSize, modulation_offset);

            size_t grain_size = calculate_grain_size(grain_size_param, size_temperature);

            float velocity = get_parameter<float>(Velocity, modulation_offset);
            float const velocity_temperature =
                std::clamp(get_parameter<float>(TemperatureVelocity, modulation_offset),
                           0.0f,
                           1.0f);
            velocity = calculate_velocity(velocity, velocity_temperature);
            // Modulation can drive velocity to zero or negative, which would
            // produce empty grains (silence) via the size maths below.
            velocity = std::max(velocity, 0.01f);

            // Apply sample start/end/loop region
            size_t const total_frames = audio_data[sample_index].get_num_samples();
            auto region = compute_sample_region(total_frames, modulation_offset);
            if (region.size() == 0) { return; }

            auto effective_grain_size =
                static_cast<size_t>(std::ceil(static_cast<float>(grain_size) * velocity));

            float const position_temperature = apply_temperature_ramp(
                std::clamp(get_parameter<float>(TemperaturePosition, modulation_offset),
                           0.0f,
                           1.0f));
            long const start_position =
                calculate_start_position(region, position_temperature, modulation_offset);

            // Truncate grain if it would overshoot past region end
            long const end_frame = static_cast<long>(region.m_end);
            long const grain_end = start_position + static_cast<long>(effective_grain_size);
            if (grain_end > end_frame) {
                effective_grain_size =
                    static_cast<size_t>(std::max(0L, end_frame - start_position));
                if (effective_grain_size == 0) { return; }
                grain_size = std::max(
                    static_cast<size_t>(1),
                    static_cast<size_t>(static_cast<float>(effective_grain_size) / velocity));
            }

            // Setup the grain
            grain.m_start_position = start_position;
            grain.m_current_position = 0;
            grain.m_grain_size = grain_size;
            grain.m_velocity = velocity;
            grain.m_active = true;
            grain.m_sample_index = sample_index;
            grain.m_position_spread = m_uni_dist(m_random_generator);

            // Get the grain duration in milliseconds
            float const grain_duration_ms =
                (static_cast<float>(grain_size) / static_cast<float>(m_sample_rate)) * 1000.0f;

            // Configure the Hann window envelope
            grain.m_envelope.set_sample_rate(static_cast<float>(m_sample_rate));
            grain.m_envelope.set_duration(grain_duration_ms);
            grain.m_envelope.start();

            // Notify visualization listeners
            for (auto* l : m_viz_listeners) {
                auto total = static_cast<float>(total_frames);
                l->on_grain_triggered(static_cast<int>(gi),
                                      static_cast<float>(start_position) / total,
                                      static_cast<float>(effective_grain_size) / total,
                                      velocity,
                                      grain_duration_ms);
            }

            return;
        }
    }
}

size_t GrainProcessorImpl::calculate_grain_size(float grain_size_param, float temperature) {
    // Both inputs are modulated and may arrive outside [0, 1]; out-of-range
    // values would underflow the size_t casts below.
    grain_size_param = std::clamp(grain_size_param, 0.0f, 1.0f);
    temperature = std::clamp(temperature, 0.0f, 1.0f);
    auto min_size = static_cast<size_t>(k_min_grain_size * m_sample_rate);
    auto max_size = static_cast<size_t>(k_max_grain_size * m_sample_rate);
    size_t const range = max_size - min_size;
    size_t grain_size =
        min_size + static_cast<size_t>(grain_size_param * static_cast<float>(range));

    // Randomize grain size based on temperature
    float rand_value = m_uni_dist(m_random_generator);  // [0, 1)
    rand_value = (rand_value * 2.f - 1.f) / 2.f;        // [-0.5, 0.5)
    rand_value *= std::pow(temperature, 3.f);
    auto lower_interval = static_cast<float>(grain_size - min_size);
    auto upper_interval = static_cast<float>(max_size - grain_size);
    if (rand_value < 0.f) {
        grain_size =
            static_cast<size_t>(static_cast<float>(grain_size) + lower_interval * rand_value);
    } else {
        // Do not make the grain size much larger for small values
        grain_size = static_cast<size_t>(static_cast<float>(grain_size) +
                                         upper_interval * rand_value * 0.3f +
                                         static_cast<float>(grain_size) * rand_value);
    }
    return std::clamp(grain_size, min_size, max_size);
}

long GrainProcessorImpl::calculate_start_position(const SampleRegion& region,
                                                  float temperature,
                                                  uint32_t modulation_offset) {
    long const max_position = static_cast<long>(region.size());
    if (max_position <= 0) { return static_cast<long>(region.m_start); }

    bool const positional = m_active_mode == EngineMode::GranularPosition;
    long start_position = 0;

    if (positional) {
        // The head is parked on Position. Each grain is drawn uniformly from
        // the Spray window around it — a deliberate width, biased before (-)
        // or after (+) the point by Tilt: the symmetric [-1, 1] window slides
        // to [tilt - 1, tilt + 1] and is clipped. Variation's position
        // temperature is then applied on top exactly as in Loop mode, so it
        // can push a grain outside the window.
        float const position =
            std::clamp(get_parameter<float>(Position, modulation_offset), 0.0f, 1.0f);
        float const spray = std::clamp(get_parameter<float>(Spray, modulation_offset), 0.0f, 1.0f);
        float const tilt = std::clamp(get_parameter<float>(Tilt, modulation_offset), -1.0f, 1.0f);
        float const window_lo = std::max(-1.0f, tilt - 1.0f);
        float const window_hi = std::min(1.0f, tilt + 1.0f);
        float const u = m_uni_dist(m_random_generator);  // [0, 1)
        float const spray_offset = (window_lo + u * (window_hi - window_lo)) * spray;
        start_position = static_cast<long>(position * static_cast<float>(max_position - 1)) +
                         static_cast<long>(spray_offset * static_cast<float>(max_position));
    } else {
        start_position = static_cast<long>(m_sequential_position);
        if (temperature == 0.f && start_position >= max_position) {
            return static_cast<long>(region.m_loop_point);
        }
    }

    // Apply temperature-based randomization to grain start position
    float rand_value = m_uni_dist(m_random_generator);  // [0, 1)
    rand_value = (rand_value - 0.5f) * 2.f;             // [-1, 1)
    rand_value *= temperature;
    start_position += static_cast<long>(rand_value * static_cast<float>(max_position));
    // Spray + temperature can overshoot by up to two region lengths; wrap
    // however many times it takes.
    while (start_position >= max_position) { start_position -= max_position; }
    while (start_position < 0) { start_position += max_position; }

    start_position += static_cast<long>(region.m_start);

    if (!positional) {
        // Advance sequential position and handle looping.
        // Scanner traverses the full region (start → end), then restarts at loop_point.
        m_sequential_position += static_cast<long>(m_min_grain_interval);

        if (std::cmp_greater_equal(m_sequential_position, max_position)) {
            m_sequential_position = static_cast<long>(region.m_loop_point - region.m_start);
        }
    }

    return start_position;
}

float GrainProcessorImpl::calculate_velocity(float velocity, float temperature) {
    float velocity_factor = m_uni_dist(m_random_generator);  // [0, 1)
    velocity_factor = (velocity_factor * 2.f - 1.f);         // [-1, 1)
    velocity_factor *= std::pow(temperature, 15.f);
    float const semitone_factor = std::pow(2.f, 7.f / 12.f);
    if (velocity_factor < 0.f) {
        velocity /= (1.f - velocity_factor * (semitone_factor - 1.f));
    } else {
        velocity *= (1.f + velocity_factor * (semitone_factor - 1.f));
    }
    return velocity;
}

float GrainProcessorImpl::apply_temperature_ramp(float temperature) const {
    auto ramp_samples = static_cast<float>(k_temperature_ramp_duration * m_sample_rate);
    float ramp_factor = 1.0f;
    if (static_cast<float>(m_playback_elapsed_samples) < ramp_samples) {
        ramp_factor = std::sin((static_cast<float>(m_playback_elapsed_samples) / ramp_samples) *
                               std::numbers::pi_v<float> / 2.0f);
    }
    // Blend between ramped and unramped: low temperature → ramp active, high →
    // ramp bypassed
    float const ramped = temperature * ramp_factor;
    return ramped + (temperature - ramped) * temperature;
}

GrainProcessorImpl::SampleRegion GrainProcessorImpl::compute_sample_region(
    size_t total_frames,
    uint32_t modulation_offset) {
    // Position mode has no travelling head, so nothing to bound or return
    // to: Position is absolute in the sample and the spray wraps across all
    // of it. Start / Loop / End stay dormant (no hidden state on a switch).
    if (m_active_mode == EngineMode::GranularPosition) { return SampleRegion{0, total_frames, 0}; }
    auto total_f = static_cast<float>(total_frames);
    // Clamp the normalized positions before the size_t casts: modulation may
    // push them negative, and a negative float -> size_t cast is UB (on x86 it
    // wraps, collapsing the region to zero and muting the voice).
    auto norm = [this, modulation_offset](Parameter p) {
        return std::clamp(get_parameter_float(p, modulation_offset), 0.0f, 1.0f);
    };
    auto start = static_cast<size_t>(norm(SampleStart) * total_f);
    auto end = static_cast<size_t>(norm(SampleEnd) * total_f);
    auto loop = static_cast<size_t>(norm(SampleLoopPoint) * total_f);
    start = std::clamp(start, static_cast<size_t>(0), total_frames);
    end = std::clamp(end, start, total_frames);
    loop = std::clamp(loop, start, end);
    return SampleRegion{start, end, loop};
}

void GrainProcessorImpl::read_sample(float position,
                                     size_t sample_index,
                                     size_t source_channel,
                                     float& out_sample) {
    out_sample = 0.0f;

    const auto& audio_data = m_audio_store.get_buffer();

    // Bounds checking
    if (sample_index >= audio_data.size() || audio_data[sample_index].empty()) { return; }

    const auto& buf = audio_data[sample_index];
    if (source_channel >= buf.get_num_channels()) { return; }

    size_t const num_frames = buf.get_num_samples();

    // Linear interpolation between samples for fractional positions
    long pos_floor = static_cast<long>(position);
    long pos_ceil = pos_floor + 1;
    float const frac = position - static_cast<float>(pos_floor);

    // Ensure we don't read beyond the buffer
    while (std::cmp_greater_equal(pos_ceil, num_frames)) {
        pos_ceil -= static_cast<long>(num_frames);
    }

    while (std::cmp_greater_equal(pos_floor, num_frames)) {
        pos_floor -= static_cast<long>(num_frames);
    }

    const float* ch_data = buf.get_read_pointer(source_channel);
    out_sample = ch_data[pos_floor] * (1.0f - frac) + ch_data[pos_ceil] * frac;
}

void GrainProcessorImpl::read_sample_exact(double position,
                                           size_t sample_index,
                                           size_t source_channel,
                                           float& out_sample) {
    out_sample = 0.0f;

    const auto& audio_data = m_audio_store.get_buffer();
    if (sample_index >= audio_data.size() || audio_data[sample_index].empty()) { return; }

    const auto& buf = audio_data[sample_index];
    if (source_channel >= buf.get_num_channels()) { return; }

    size_t const num_frames = buf.get_num_samples();
    if (num_frames == 0) { return; }

    // Clamp (never wrap): the loop-fade head deliberately runs past the
    // region end and must hold the sample's last frame, not alias back onto
    // the frames the incoming head is playing.
    double const pos = std::clamp(position, 0.0, static_cast<double>(num_frames - 1));
    auto const frame_a = static_cast<size_t>(pos);
    size_t const frame_b = std::min(frame_a + 1, num_frames - 1);
    float const frac = static_cast<float>(pos - static_cast<double>(frame_a));

    const float* ch_data = buf.get_read_pointer(source_channel);
    out_sample = ch_data[frame_a] * (1.0f - frac) + ch_data[frame_b] * frac;
}

}  // namespace thl::dsp::granular
