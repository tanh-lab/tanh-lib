#include <tanh/core/BufferView.h>
#include <tanh/dsp/audio/AudioDataStore.h>
#include <tanh/dsp/granular/ChannelMixer.h>
#include <tanh/dsp/granular/GrainEngine.h>
#include <tanh/dsp/granular/GrainProcessor.h>
#include <tanh/dsp/granular/GrainVisualizationListener.h>
#include <tanh/dsp/granular/GranularTypes.h>
#include <tanh/dsp/granular/VoiceParams.h>
#include <tanh/dsp/sampler/SamplePlayer.h>
#include <tanh/dsp/sampler/SampleView.h>
#include <tanh/dsp/slicing/SliceSteps.h>
#include <tanh/dsp/utils/MorphWindow.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace thl::dsp::granular {

namespace {

HeadMode head_mode(EngineMode mode) {
    return mode == EngineMode::GranularPosition ? HeadMode::Spray : HeadMode::Scan;
}

}  // namespace

GrainProcessorImpl::GrainProcessorImpl(audio::AudioDataStore& audio_store)
    : m_audio_store(audio_store) {
    m_grain_engine.set_visualizer(&m_viz);
    m_sources.reserve(k_max_sources);
}

GrainProcessorImpl::~GrainProcessorImpl() = default;

void GrainProcessorImpl::set_visualization_listener(GrainVisualizationListener* listener) {
    m_viz.set_listener(listener);
}

void GrainProcessorImpl::add_visualization_listener(GrainVisualizationListener* listener) {
    m_viz.add_listener(listener);
}

void GrainProcessorImpl::remove_visualization_listener(GrainVisualizationListener* listener) {
    m_viz.remove_listener(listener);
}

void GrainProcessorImpl::set_visualization_update_rate(float fps) {
    m_viz.set_update_rate(m_sample_rate, fps);
}

void GrainProcessorImpl::reset_grains() {
    m_grain_engine.deactivate_all();
    m_grain_engine.reset_schedule(head_mode(m_active_mode));
    m_last_playing_state = false;
    m_was_sounding = false;
    m_playback_elapsed_samples = 0;
    m_envelope.reset();
    reset_player();
    m_mode_fade_out = false;
    m_mode_gain = 1.0f;
    m_one_shot_done = false;
}

void GrainProcessorImpl::prepare(const double& sample_rate,
                                 const size_t& samples_per_block,
                                 const size_t& num_channels) {
    m_sample_rate = sample_rate;
    m_channels = std::min(num_channels, k_max_channel_support);

    m_grain_engine.prepare(sample_rate, m_channels);
    m_viz.player_resetting(m_player);
    m_player.prepare(sample_rate);
    m_scratch_frames = std::max<size_t>(1, samples_per_block);
    m_head_scratch.assign(k_max_channel_support * m_scratch_frames, 0.0f);
    m_mix_scratch.assign(k_max_channel_support * m_scratch_frames, 0.0f);
    m_sources_valid = false;

    // Mode timing. The active mode seeds from the parameter so a preset that
    // boots in Sample mode doesn't run one block of grains first.
    m_mode_gain_step =
        1.0f / std::max(1.0f, k_mode_change_fade_duration * static_cast<float>(m_sample_rate));
    m_active_mode =
        static_cast<EngineMode>(std::clamp(get_parameter<int>(EngineModeParam),
                                           0,
                                           static_cast<int>(EngineMode::NumEngineModes) - 1));
    m_mode_gain = 1.0f;
    m_mode_fade_out = false;

    // Seed the volume ramp at the current level: a voice must not fade in from
    // zero on its first block just because the smoother starts there.
    m_volume_smoother.reset(m_sample_rate, k_volume_smoothing_duration);
    m_volume_smoother.set_current_and_target_value(get_parameter<float>(Volume));

    m_envelope.set_sample_rate(static_cast<float>(m_sample_rate));
    m_envelope.set_parameters(get_parameter<float>(EnvelopeAttack),
                              get_parameter<float>(EnvelopeDecay),
                              get_parameter<float>(EnvelopeSustain),
                              get_parameter<float>(EnvelopeRelease),
                              get_parameter<float>(EnvelopeAttackCurve),
                              get_parameter<float>(EnvelopeDecayCurve),
                              get_parameter<float>(EnvelopeReleaseCurve));
    // A re-prepare (new sample rate / channel count) starts from silence.
    reset_grains();
}

VoiceParams GrainProcessorImpl::read_params(uint32_t offset) {
    // The modulation system does not promise finite values; a NaN would
    // reach size_t casts (UB) and the trigger clock. Fall back per field.
    auto finite = [this, offset](Parameter p, float fallback) {
        float const v = get_parameter<float>(p, offset);
        return std::isfinite(v) ? v : fallback;
    };
    auto unit = [&finite](Parameter p) { return std::clamp(finite(p, 0.0f), 0.0f, 1.0f); };
    VoiceParams v;
    v.m_playing = get_parameter<bool>(Playing, offset);
    v.m_volume = finite(Volume, 0.0f);
    v.m_size = unit(Size);
    v.m_density = unit(Density);
    v.m_velocity = finite(Velocity, 1.0f);
    v.m_temperature_size = unit(TemperatureSize);
    v.m_temperature_position = unit(TemperaturePosition);
    v.m_temperature_velocity = unit(TemperatureVelocity);
    v.m_sample_index = get_parameter<int>(SampleIndex, offset);
    v.m_sample_start = unit(SampleStart);
    v.m_sample_end = unit(SampleEnd);
    v.m_sample_loop_point = unit(SampleLoopPoint);
    v.m_channel_mode =
        static_cast<ChannelMode>(std::clamp(get_parameter<int>(ChannelModeParam, offset),
                                            0,
                                            static_cast<int>(ChannelMode::NumChannelModes) - 1));
    v.m_spread = unit(Spread);
    v.m_engine_mode =
        static_cast<EngineMode>(std::clamp(get_parameter<int>(EngineModeParam, offset),
                                           0,
                                           static_cast<int>(EngineMode::NumEngineModes) - 1));
    v.m_position = unit(Position);
    v.m_spray = unit(Spray);
    v.m_tilt = std::clamp(finite(Tilt, 0.0f), -1.0f, 1.0f);
    v.m_loop = get_parameter<bool>(LoopEnabled, offset);
    v.m_loop_snap = get_parameter<bool>(LoopSnap, offset);
    // Slicing needs both the flag and a valid map; otherwise every consumer
    // takes its unsliced path.
    v.m_slicer = false;
    if (get_parameter<bool>(SlicerEnabled, offset) && read_slice_map(v.m_slices) &&
        v.m_slices.valid()) {
        v.m_slicer = true;
    }
    v.m_window_shape =
        std::clamp(finite(GrainWindowShape, 4.0f), 0.0f, utils::MorphWindow::k_max_shape);
    v.m_window_tilt = std::clamp(finite(GrainWindowTilt, 0.0f), -1.0f, 1.0f);
    v.m_env_attack = finite(EnvelopeAttack, 0.0f);
    v.m_env_decay = finite(EnvelopeDecay, 0.0f);
    v.m_env_sustain = finite(EnvelopeSustain, 1.0f);
    v.m_env_release = finite(EnvelopeRelease, 0.0f);
    v.m_env_attack_curve = finite(EnvelopeAttackCurve, 0.0f);
    v.m_env_decay_curve = finite(EnvelopeDecayCurve, 0.0f);
    v.m_env_release_curve = finite(EnvelopeReleaseCurve, 0.0f);
    return v;
}

void GrainProcessorImpl::process(thl::core::BufferView buffer, uint32_t modulation_offset) {
    const AudioBlock block = begin_block(buffer);
    const VoiceParams params = read_params(modulation_offset);

    update_envelope(params);
    update_mode_fade(params);
    handle_gate(params);

    if (!is_sounding()) {
        // Once per idle stretch, not every block: idle voices would
        // otherwise scan the pool and ping the visualiser 100 times a second.
        if (m_was_sounding) { silence(); }
        m_was_sounding = false;
        return;
    }
    m_was_sounding = true;

    render_engine(block, params);
    apply_voice_gain(block, params);
    report_visualization();
}

AudioBlock GrainProcessorImpl::begin_block(thl::core::BufferView buffer) {
    AudioBlock block;
    block.m_num_frames = buffer.get_num_samples();
    block.m_num_channels =
        std::min(buffer.get_num_channels(), static_cast<size_t>(k_max_channel_support));
    for (size_t ch = 0; ch < block.m_num_channels; ++ch) {
        block.m_channels[ch] = buffer.get_write_pointer(ch);
        std::memset(block.m_channels[ch], 0, block.m_num_frames * sizeof(float));
    }
    return block;
}

void GrainProcessorImpl::handle_gate(const VoiceParams& params) {
    bool const envelope_active = m_envelope.is_active();
    if (!params.m_playing) { m_one_shot_done = false; }
    // A rising gate always starts a note. A held gate re-arms only when the
    // envelope has died on its own — not when a one-shot released it.
    bool const rising = params.m_playing && !m_last_playing_state;
    bool const rearm = params.m_playing && !envelope_active && !m_one_shot_done;
    if (rising || rearm) {
        m_one_shot_done = false;
        m_envelope.note_on();
        // A voice starts at its level, it does not slide up to it — the ADSR
        // is what shapes the onset. Without this the smoother would ramp from
        // whatever the last note left behind (or from the prepare()-time
        // volume), bending the first few ms of every note.
        m_volume_smoother.set_current_and_target_value(params.m_volume);
        m_grain_engine.reset_schedule(head_mode(m_active_mode));
        m_playback_elapsed_samples = 0;
        // Legato (crossfaded restart) only while the previous note still
        // sounds; a head whose envelope has already died restarts cold.
        if (envelope_active) {
            m_player.note_on();
        } else {
            reset_player();
        }
    } else if (!params.m_playing && m_envelope.get_state() != utils::ADSR::State::IDLE &&
               m_envelope.get_state() != utils::ADSR::State::RELEASE) {
        m_envelope.note_off();
    }
    m_last_playing_state = params.m_playing;
}

bool GrainProcessorImpl::is_sounding() const {
    return m_envelope.is_active() && m_audio_store.is_loaded();
}

void GrainProcessorImpl::silence() {
    m_grain_engine.deactivate_all();
    m_viz.set_master_level(0.f);
    m_viz.report_master_level();
    reset_player();
}

void GrainProcessorImpl::reset_player() {
    m_viz.player_resetting(m_player);
    m_player.reset();
}

void GrainProcessorImpl::refresh_sources() {
    uint32_t const generation = m_audio_store.get_load_generation();
    if (m_sources_valid && generation == m_sources_generation) { return; }
    // Within the capacity reserved at construction: no allocation.
    const auto& banks = m_audio_store.get_buffer();
    size_t const count = std::min(banks.size(), k_max_sources);
    m_sources.resize(count);
    for (size_t i = 0; i < count; ++i) { m_sources[i] = sampler::SampleView::of(banks[i]); }
    m_sources_generation = generation;
    m_sources_valid = true;
    // New audio may reuse an old buffer's address and length.
    m_player.sources_changed();
}

void GrainProcessorImpl::render_engine(const AudioBlock& block, const VoiceParams& params) {
    m_viz.set_master_level(m_envelope.get_current_level());
    refresh_sources();
    bool rendered = true;
    bool finished = false;
    if (m_active_mode == EngineMode::Sample) {
        rendered = render_player(block, params);
        finished = m_player.finished();
    } else {
        size_t const source =
            static_cast<size_t>(std::clamp(params.m_sample_index,
                                           0,
                                           std::max(0, static_cast<int>(m_sources.size()) - 1)));
        GrainParams grain;
        grain.m_size = params.m_size;
        grain.m_density = params.m_density;
        grain.m_pitch = params.m_velocity;
        grain.m_temperature_size = params.m_temperature_size;
        grain.m_temperature_position = params.m_temperature_position;
        grain.m_temperature_pitch = params.m_temperature_velocity;
        grain.m_window_shape = params.m_window_shape;
        grain.m_window_tilt = params.m_window_tilt;
        grain.m_channel_mode = params.m_channel_mode;
        grain.m_spread = params.m_spread;
        grain.m_head = {.m_start = params.m_sample_start,
                        .m_end = params.m_sample_end,
                        .m_loop_point = params.m_sample_loop_point,
                        .m_loop = params.m_loop,
                        .m_position = params.m_position,
                        .m_spray = params.m_spray,
                        .m_tilt = params.m_tilt,
                        .m_slices = params.m_slicer ? &params.m_slices : nullptr};
        HeadMode const mode = head_mode(m_active_mode);
        m_grain_engine.render(m_sources,
                              source,
                              grain,
                              mode,
                              block.m_channels.data(),
                              block.m_num_channels,
                              block.m_num_frames,
                              m_playback_elapsed_samples);
        finished = m_grain_engine.finished(mode);
    }
    // The temperature ramp counts sounding time only.
    if (rendered) { m_playback_elapsed_samples += block.m_num_frames; }
    // One-shot reached End: release the voice once. The latch keeps
    // handle_gate from re-triggering while the gate is still held.
    if (finished && !m_one_shot_done) {
        m_one_shot_done = true;
        if (m_envelope.get_state() != utils::ADSR::State::IDLE &&
            m_envelope.get_state() != utils::ADSR::State::RELEASE) {
            m_envelope.note_off();
        }
    }
}

bool GrainProcessorImpl::render_player(const AudioBlock& block, const VoiceParams& params) {
    size_t const source = static_cast<size_t>(
        std::clamp(params.m_sample_index, 0, std::max(0, static_cast<int>(m_sources.size()) - 1)));
    const sampler::SampleView* view = source < m_sources.size() ? &m_sources[source] : nullptr;
    size_t const source_channels = view != nullptr ? view->m_num_channels : 0;
    // The player renders the source's own channels (at least a stereo pair,
    // a mono source duplicated); the channel mode then maps them out.
    size_t const head_channels =
        std::min(k_max_channel_support, std::max<size_t>(2, source_channels));

    if (params.m_slicer && view != nullptr) {
        m_player.set_region(slicing::region_from_steps(params.m_slices,
                                                       params.m_sample_start,
                                                       params.m_sample_end,
                                                       view->m_num_frames));
    } else {
        m_player.set_markers(params.m_sample_start,
                             params.m_sample_end,
                             params.m_sample_loop_point);
    }
    m_player.set_speed(params.m_velocity);
    m_player.set_loop(params.m_loop);
    m_player.set_snap(params.m_loop_snap);

    std::array<float*, k_max_channel_support> head{};
    std::array<float*, k_max_channel_support> mix{};
    size_t const write_channels = std::min(m_channels, block.m_num_channels);
    bool rendered_any = false;
    for (size_t done = 0; done < block.m_num_frames;) {
        size_t const frames = std::min(m_scratch_frames, block.m_num_frames - done);
        for (size_t ch = 0; ch < k_max_channel_support; ++ch) {
            head[ch] = m_head_scratch.data() + ch * m_scratch_frames;
            mix[ch] = m_mix_scratch.data() + ch * m_scratch_frames;
        }
        bool const was_started = m_player.started();
        bool const rendered =
            m_player.render(m_sources, source, head.data(), head_channels, frames);
        m_viz.player_rendered(m_player, was_started, rendered, m_sample_rate);
        if (!rendered) { return rendered_any; }
        rendered_any = true;
        channel_mixer::mix_head(head.data(),
                                source_channels,
                                params.m_channel_mode,
                                params.m_spread,
                                mix.data(),
                                m_channels,
                                frames);
        for (size_t ch = 0; ch < write_channels; ++ch) {
            std::copy_n(mix[ch], frames, block.m_channels[ch] + done);
        }
        done += frames;
    }
    return rendered_any;
}

void GrainProcessorImpl::apply_voice_gain(const AudioBlock& block, const VoiceParams& params) {
    // Master volume, ADSR and the mode-change fade. The fade ramps linearly
    // toward 0 while a mode switch is pending and back to 1 after the switch
    // has happened (see update_mode_fade).
    // Volume arrives as a sub-block constant; ramp toward it per sample so a
    // modulated step (a square LFO swings both rails in one sample) reaches the
    // output as a short slope instead of a discontinuity.
    m_volume_smoother.set_target_value(params.m_volume);

    float const mode_target = m_mode_fade_out ? 0.0f : 1.0f;

    // A mode switch is rare and short; almost every block runs with the fade
    // already parked on its target, where the ramp below is a no-op that still
    // costs two compares and a store per frame. Split it out and the common
    // block is a plain gain pass.
    if (m_mode_gain == mode_target) {
        float const mode_gain = m_mode_gain;
        for (size_t i = 0; i < block.m_num_frames; i++) {
            float const gain =
                m_volume_smoother.get_smoothed_value() * m_envelope.process() * mode_gain;
            for (size_t ch = 0; ch < block.m_num_channels; ++ch) {
                block.m_channels[ch][i] *= gain;
            }
        }
        return;
    }

    for (size_t i = 0; i < block.m_num_frames; i++) {
        if (m_mode_gain < mode_target) {
            m_mode_gain = std::min(mode_target, m_mode_gain + m_mode_gain_step);
        } else if (m_mode_gain > mode_target) {
            m_mode_gain = std::max(mode_target, m_mode_gain - m_mode_gain_step);
        }
        float const gain =
            m_volume_smoother.get_smoothed_value() * m_envelope.process() * m_mode_gain;
        for (size_t ch = 0; ch < block.m_num_channels; ++ch) { block.m_channels[ch][i] *= gain; }
    }
}

void GrainProcessorImpl::report_visualization() {
    // The head reports AFTER the gain pass so the level is this block's,
    // not the previous one's. Grains report from inside GrainEngine::render
    // on their own rate-limited cadence.
    if (m_active_mode == EngineMode::Sample) {
        m_viz.set_master_level(m_envelope.get_current_level());
        m_viz.player_updated(m_player);
    }
}

void GrainProcessorImpl::update_mode_fade(const VoiceParams& params) {
    EngineMode const requested = params.m_engine_mode;
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
    m_grain_engine.deactivate_all();
    m_grain_engine.reset_schedule(head_mode(m_active_mode));
    reset_player();
    // A finished one-shot in the old mode must not keep the new one silent.
    m_one_shot_done = false;
}

void GrainProcessorImpl::update_envelope(const VoiceParams& p) {
    m_envelope.set_parameters(p.m_env_attack,
                              p.m_env_decay,
                              p.m_env_sustain,
                              p.m_env_release,
                              p.m_env_attack_curve,
                              p.m_env_decay_curve,
                              p.m_env_release_curve);
}

}  // namespace thl::dsp::granular
