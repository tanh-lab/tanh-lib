#include <tanh/core/BufferView.h>
#include <tanh/dsp/audio/AudioDataStore.h>
#include <tanh/dsp/granular/GrainProcessor.h>
#include <tanh/dsp/granular/GrainVisualizationListener.h>
#include <tanh/dsp/granular/GranularTypes.h>
#include <tanh/dsp/granular/VoiceParams.h>
#include <tanh/dsp/utils/MorphWindow.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace thl::dsp::granular {

GrainProcessorImpl::GrainProcessorImpl(audio::AudioDataStore& audio_store)
    : m_audio_store(audio_store)
    , m_reader(audio_store)
    , m_grain_engine(m_reader, m_viz)
    , m_player(m_reader, m_viz) {}

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
    m_grain_engine.reset_schedule(m_active_mode);
    m_last_playing_state = false;
    m_was_sounding = false;
    m_playback_elapsed_samples = 0;
    m_envelope.reset();
    m_player.reset();
    m_mode_fade_out = false;
    m_mode_gain = 1.0f;
}

void GrainProcessorImpl::prepare(const double& sample_rate,
                                 const size_t& /*samples_per_block*/,
                                 const size_t& num_channels) {
    m_sample_rate = sample_rate;
    m_channels = std::min(num_channels, k_max_channel_support);

    m_grain_engine.prepare(sample_rate, m_channels);
    m_player.prepare(sample_rate, m_channels);

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
    if (params.m_playing && (!envelope_active || !m_last_playing_state)) {
        m_envelope.note_on();
        m_grain_engine.reset_schedule(m_active_mode);
        m_playback_elapsed_samples = 0;
        // Legato (crossfaded restart) only while the previous note still
        // sounds; a head whose envelope has already died restarts cold.
        if (envelope_active) {
            m_player.note_on();
        } else {
            m_player.reset();
        }
    } else if (!params.m_playing && m_envelope.get_state() != utils::ADSR::State::IDLE &&
               m_envelope.get_state() != utils::ADSR::State::RELEASE) {
        m_envelope.note_off();
    }
    m_last_playing_state = params.m_playing;
}

bool GrainProcessorImpl::is_sounding() const {
    return m_envelope.is_active() && m_reader.is_loaded();
}

void GrainProcessorImpl::silence() {
    m_grain_engine.deactivate_all();
    m_viz.set_master_level(0.f);
    m_viz.report_master_level();
    m_player.reset();
}

void GrainProcessorImpl::render_engine(const AudioBlock& block, const VoiceParams& params) {
    m_viz.set_master_level(m_envelope.get_current_level());
    bool rendered = true;
    if (m_active_mode == EngineMode::Sample) {
        rendered = m_player.render(block, params);
    } else {
        m_grain_engine.render(block, params, m_active_mode, m_playback_elapsed_samples);
    }
    // The temperature ramp counts sounding time only.
    if (rendered) { m_playback_elapsed_samples += block.m_num_frames; }
}

void GrainProcessorImpl::apply_voice_gain(const AudioBlock& block, const VoiceParams& params) {
    // Master volume, ADSR and the mode-change fade. The fade ramps linearly
    // toward 0 while a mode switch is pending and back to 1 after the switch
    // has happened (see update_mode_fade).
    float const mode_target = m_mode_fade_out ? 0.0f : 1.0f;
    for (size_t i = 0; i < block.m_num_frames; i++) {
        if (m_mode_gain < mode_target) {
            m_mode_gain = std::min(mode_target, m_mode_gain + m_mode_gain_step);
        } else if (m_mode_gain > mode_target) {
            m_mode_gain = std::max(mode_target, m_mode_gain - m_mode_gain_step);
        }
        float const gain = params.m_volume * m_envelope.process() * m_mode_gain;
        for (size_t ch = 0; ch < block.m_num_channels; ++ch) { block.m_channels[ch][i] *= gain; }
    }
}

void GrainProcessorImpl::report_visualization() {
    // The head reports AFTER the gain pass so the level is this block's,
    // not the previous one's. Grains report from inside GrainEngine::render
    // on their own rate-limited cadence.
    if (m_active_mode == EngineMode::Sample) {
        m_viz.set_master_level(m_envelope.get_current_level());
        m_player.report_visualization();
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
    m_grain_engine.reset_schedule(m_active_mode);
    m_player.reset();
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
