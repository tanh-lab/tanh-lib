#pragma once

#include <tanh/core/Exports.h>
#include <tanh/dsp/BaseProcessor.h>
#include <tanh/dsp/audio/AudioDataStore.h>
#include <tanh/dsp/granular/GrainVisualizationListener.h>
#include <tanh/dsp/utils/ADSR.h>
#include <tanh/dsp/utils/HannWindow.h>

#include <array>
#include <random>
#include <vector>

namespace thl::dsp::granular {

// Maximum number of output channels supported by the GrainProcessor
constexpr size_t k_max_channel_support = 16;

// Grain size limits in seconds (will be converted to samples based on sample
// rate)
constexpr float k_min_grain_size = 0.002f;  // 2 ms
constexpr float k_max_grain_size = 0.4f;    // 400 ms

// Grain trigger rate limits in grains per second (for density control).
// Density maps exponentially between these so the control feels even.
constexpr float k_min_grain_rate = 2.0f;
constexpr float k_max_grain_rate = 100.0f;

// Maximum number of grains that can be active simultaneously.
// Must cover the worst-case overlap: k_max_grain_rate * k_max_grain_size.
constexpr size_t k_max_grains = 48;

// Duration in seconds over which temperature ramps up from 0 to full at
// playback start
constexpr float k_temperature_ramp_duration = 1.0f;

enum class ChannelMode : int {
    MonoToStereo,      // Read ch0 from source, spread across L/R
    TrueStereo,        // Read ch0+ch1 from source (mono duplicated if source is mono)
    TrueMultichannel,  // Read all source channels, write to matching output
                       // channels
    NumChannelModes
};

// Where the read head goes. All three modes share the same source buffer,
// pitch bank, region maths, ADSR and channel handling; they differ only in
// the head policy (see process()).
enum class EngineMode : int {
    GranularPosition,  // Grains sprayed around a fixed Position (no travelling head)
    GranularLoop,      // Scan head runs Start -> End at 1x, restarts at Loop
    Sample,            // One continuous interpolating head at Velocity, no grains
    NumEngineModes
};

// Length of the equal-power crossfade the Sample head runs at a loop wrap
// or a pitch-bank switch (seconds).
constexpr float k_player_crossfade_duration = 0.010f;
// Fade-through-zero when the engine mode changes on a sounding voice.
constexpr float k_mode_change_fade_duration = 0.015f;

// Structure to represent a single grain
struct Grain {
    size_t m_start_position;    // Starting position in the sample
    size_t m_current_position;  // Current position within the grain
    size_t m_grain_size;        // Size of the grain in samples
    float m_velocity;           // Playback speed/velocity
    float m_amplitude;          // Grain amplitude/volume
    float m_gain;
    float m_position_spread;                 // Pan position [0, 1] for MonoToStereo spread
    bool m_active;                           // Whether the grain is currently active
    thl::dsp::utils::HannWindow m_envelope;  // Hann window envelope for amplitude
                                             // modulation
    size_t m_sample_index;                   // Index of the sample in the audio data
};

class TANH_API GrainProcessorImpl : public thl::dsp::BaseProcessor {
public:
    explicit GrainProcessorImpl(thl::dsp::audio::AudioDataStore& audio_store);
    ~GrainProcessorImpl() override;

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
    thl::dsp::utils::ADSR m_envelope;
    thl::dsp::audio::AudioDataStore& m_audio_store;

    struct SampleRegion {
        SampleRegion(size_t start, size_t end, size_t loop_point)
            : m_start(start), m_end(end), m_loop_point(loop_point) {}
        size_t size() const { return m_end - m_start; }

    private:
        size_t m_start;
        size_t m_end;
        size_t m_loop_point;
        friend class GrainProcessorImpl;
    };

    // Template wrapper for get_parameter
    template <typename T>
    T get_parameter(Parameter parameter, uint32_t modulation_offset = 0);

    virtual float get_parameter_float(Parameter parameter, uint32_t modulation_offset = 0) = 0;
    virtual bool get_parameter_bool(Parameter parameter, uint32_t modulation_offset = 0) = 0;
    virtual int get_parameter_int(Parameter parameter, uint32_t modulation_offset = 0) = 0;

    double m_sample_rate = 48000.0;
    size_t m_channels = 2;
    // Channels actually filled for the current block (a device switch can
    // hand us fewer channels than prepare() promised; writing to m_channels
    // would dereference null pointers).
    size_t m_block_channels = 2;

    // Grain management
    std::vector<Grain> m_grains;
    size_t m_max_grains{k_max_grains};
    size_t m_next_grain_time{0};
    size_t m_min_grain_interval{100};
    size_t m_sequential_position{0};

    // Random number generation for grain parameters
    std::mt19937 m_random_generator;
    std::uniform_real_distribution<float> m_uni_dist;

    // Envelope
    bool m_last_playing_state{false};
    size_t m_playback_elapsed_samples{0};
    float m_last_envelope_attack{-1.0f};
    float m_last_envelope_decay{-1.0f};
    float m_last_envelope_sustain{-1.0f};
    float m_last_envelope_release{-1.0f};
    float m_last_envelope_attack_curve{-2.0f};
    float m_last_envelope_decay_curve{-2.0f};
    float m_last_envelope_release_curve{-2.0f};
    void update_envelope_if_needed(uint32_t modulation_offset);

    // Sample index management
    size_t m_current_sample_index{0};

    // Engine mode. The active mode only changes once the mode-change fade has
    // reached silence, so a switch on a sounding voice never clicks.
    EngineMode m_active_mode{EngineMode::GranularLoop};
    float m_mode_gain{1.0f};
    float m_mode_gain_step{1.0f};
    bool m_mode_fade_out{false};

    // Sample-mode read head. `m_play_head` is the live position in source
    // frames; while `m_fade_remaining > 0` a second head (`m_fade_head`,
    // reading `m_fade_sample_index`) rides out the crossfade.
    double m_play_head{0.0};
    double m_fade_head{0.0};
    size_t m_fade_sample_index{0};
    size_t m_fade_remaining{0};
    size_t m_fade_length{0};
    size_t m_player_sample_index{0};
    size_t m_player_total_frames{0};  // for viz normalization after the block
    bool m_player_started{false};
    void render_player(float** buffer, size_t n_buffer_frames, uint32_t modulation_offset);
    void start_player_crossfade(size_t old_sample_index);
    void read_player_frame(double position,
                           size_t sample_index,
                           size_t source_channels,
                           ChannelMode mode,
                           float width,
                           std::array<float, k_max_channel_support>& out);
    void update_mode_fade(uint32_t modulation_offset);
    void reset_player();

    // Grain generation and management
    void trigger_grain(const size_t sample_index, uint32_t modulation_offset);
    void update_grains(float** buffer, size_t n_buffer_frames, uint32_t modulation_offset);
    void read_sample(float position, size_t sample_index, size_t source_channel, float& out_sample);
    // Player variant: full double-precision addressing (a float head loses
    // sample-exact positions past ~2^24 frames), and clamped to the last
    // frame instead of wrapping — the outgoing loop-fade head must never fold
    // back onto the material the incoming head is playing.
    void read_sample_exact(double position,
                           size_t sample_index,
                           size_t source_channel,
                           float& out_sample);
    size_t calculate_grain_size(float grain_size_param, float temperature);
    float calculate_velocity(float velocity, float temperature);
    long calculate_start_position(const SampleRegion& region,
                                  float temperature,
                                  uint32_t modulation_offset);
    float apply_temperature_ramp(float temperature) const;
    SampleRegion compute_sample_region(size_t total_frames, uint32_t modulation_offset);

    // Visualization listeners (optional, not owned)
    std::vector<GrainVisualizationListener*> m_viz_listeners;
    size_t m_viz_update_interval = 0;  // in samples, 0 = disabled
    size_t m_viz_update_counter = 0;
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
