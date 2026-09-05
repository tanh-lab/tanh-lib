#pragma once

#include <tanh/core/Exports.h>
#include <tanh/dsp/BaseProcessor.h>
#include <tanh/dsp/audio/AudioDataStore.h>
#include <tanh/dsp/granular/GrainEngine.h>
#include <tanh/dsp/granular/GrainVisualizationListener.h>
#include <tanh/dsp/granular/GrainVisualizer.h>
#include <tanh/dsp/granular/GranularTypes.h>
#include <tanh/dsp/granular/SamplePlayer.h>
#include <tanh/dsp/granular/SampleReader.h>
#include <tanh/dsp/granular/VoiceParams.h>
#include <tanh/dsp/utils/ADSR.h>

#include <cstddef>
#include <cstdint>

namespace thl::dsp::granular {

// One granular voice: the BaseProcessor facade the host subclasses to bind
// parameters. It owns what is common to every engine mode — the parameter
// snapshot, the master ADSR and note logic, the mode-change fade — and
// dispatches each block to one of two pre-allocated engines: the
// GrainEngine (Position / Loop, told where to start grains by a HeadPolicy)
// or the SamplePlayer (Sample). The engines never see the parameter system.
class TANH_API GrainProcessorImpl : public thl::dsp::BaseProcessor {
public:
    explicit GrainProcessorImpl(thl::dsp::audio::AudioDataStore& audio_store);
    ~GrainProcessorImpl() override;
    // The engines hold references into this object: never copied.
    GrainProcessorImpl(const GrainProcessorImpl&) = delete;
    GrainProcessorImpl& operator=(const GrainProcessorImpl&) = delete;

    void prepare(const double& sample_rate,
                 const size_t& samples_per_block,
                 const size_t& num_channels) override;
    void process(thl::core::BufferView buffer, uint32_t modulation_offset = 0) override;

    void reset_grains();

    bool is_active() const { return m_envelope.is_active(); }

    void set_visualization_listener(GrainVisualizationListener* listener);
    void add_visualization_listener(GrainVisualizationListener* listener);
    void remove_visualization_listener(GrainVisualizationListener* listener);
    void set_visualization_update_rate(float fps);

protected:
    enum Parameter {
        Playing,
        Volume,

        Size,
        Density,
        Velocity,

        TemperatureSize,
        TemperaturePosition,
        TemperatureVelocity,

        SampleIndex,
        SampleStart,
        SampleEnd,
        SampleLoopPoint,

        ChannelModeParam,
        Spread,

        EngineModeParam,
        Position,
        Spray,
        Tilt,

        EnvelopeAttack,
        EnvelopeDecay,
        EnvelopeSustain,
        EnvelopeRelease,
        EnvelopeAttackCurve,
        EnvelopeDecayCurve,
        EnvelopeReleaseCurve,

        NumParameters
    };

private:
    // Template wrapper for get_parameter
    template <typename T>
    T get_parameter(Parameter parameter, uint32_t modulation_offset = 0);

    virtual float get_parameter_float(Parameter parameter, uint32_t modulation_offset = 0) = 0;
    virtual bool get_parameter_bool(Parameter parameter, uint32_t modulation_offset = 0) = 0;
    virtual int get_parameter_int(Parameter parameter, uint32_t modulation_offset = 0) = 0;

    // process() in order:
    AudioBlock begin_block(thl::core::BufferView buffer);  // pointers, clear
    // The one place the parameter system is read: once per process() call,
    // clamped into the ranges the components rely on.
    VoiceParams read_params(uint32_t modulation_offset);
    void update_envelope(const VoiceParams& params);   // hand the ADSR this block's values
    void update_mode_fade(const VoiceParams& params);  // mode-switch state machine
    void handle_gate(const VoiceParams& params);       // note-on / note-off edges
    bool is_sounding() const;
    void silence();  // envelope idle or no sample: drop grains, head, viz
    void render_engine(const AudioBlock& block, const VoiceParams& params);
    void apply_voice_gain(const AudioBlock& block, const VoiceParams& params);
    void report_visualization();

    thl::dsp::utils::ADSR m_envelope;
    thl::dsp::audio::AudioDataStore& m_audio_store;

    double m_sample_rate = 48000.0;
    size_t m_channels = 2;

    // Shared collaborators, declared before the engines that hold them.
    SampleReader m_reader;
    GrainVisualizer m_viz;
    // Both engines pre-allocated: a mode switch is a dispatch change after
    // the fade, never an allocation.
    GrainEngine m_grain_engine;
    SamplePlayer m_player;

    // Note logic
    bool m_last_playing_state{false};
    bool m_was_sounding{false};  // silence() runs once per idle stretch
    size_t m_playback_elapsed_samples{0};

    // Engine mode. The active mode only changes once the mode-change fade has
    // reached silence, so a switch on a sounding voice never clicks.
    EngineMode m_active_mode{EngineMode::GranularLoop};
    float m_mode_gain{1.0f};
    float m_mode_gain_step{1.0f};
    bool m_mode_fade_out{false};
};

// Template specializations for get_parameter
template <>
inline float GrainProcessorImpl::get_parameter<float>(Parameter p, uint32_t modulation_offset) {
    return get_parameter_float(p, modulation_offset);
}
template <>
inline bool GrainProcessorImpl::get_parameter<bool>(Parameter p, uint32_t modulation_offset) {
    return get_parameter_bool(p, modulation_offset);
}
template <>
inline int GrainProcessorImpl::get_parameter<int>(Parameter p, uint32_t modulation_offset) {
    return get_parameter_int(p, modulation_offset);
}

}  // namespace thl::dsp::granular
