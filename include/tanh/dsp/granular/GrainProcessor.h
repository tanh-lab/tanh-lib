#pragma once

#include <random>
#include <vector>
#include <tanh/tanh.h>
#include <tanh/dsp/audio/AudioDataStore.h>

namespace thl::dsp::granular {

// Maximum number of output channels supported by the GrainProcessor
constexpr size_t MAX_CHANNEL_SUPPORT = 16;

// Grain size limits in seconds (will be converted to samples based on sample rate)
constexpr float MIN_GRAIN_SIZE = 0.02f; // 20 ms
constexpr float MAX_GRAIN_SIZE = 0.4f;  // 400 ms

// Minium and maximum grain interval in seconds (for density control)
constexpr float MIN_GRAIN_INTERVAL = 0.02f; // 20 ms (50 grains/sec)
constexpr float MAX_GRAIN_INTERVAL = 0.2f;  // 200 ms (5 grains/sec)

// Maximum number of grains that can be active simultaneously
constexpr size_t MAX_GRAINS = 32;

enum class ChannelMode : int {
    MonoToStereo = 0,      // Read ch0 from source, spread across L/R
    TrueStereo = 1,            // Read ch0+ch1 from source (mono duplicated if source is mono)
    TrueMultichannel = 2,   // Read all source channels, write to matching output channels
    NUM_CHANNEL_MODES = 3
};

// Structure to represent a single grain
struct Grain {
    size_t start_position;     // Starting position in the sample
    size_t current_position;   // Current position within the grain
    size_t grain_size;         // Size of the grain in samples
    float velocity;            // Playback speed/velocity
    float amplitude;           // Grain amplitude/volume
    float gain;
    float position_spread;                 // Pan position [0, 1] for MonoToStereo spread
    bool active;               // Whether the grain is currently active
    thl::dsp::utils::HannWindow envelope;       // Hann window envelope for amplitude modulation
    size_t sample_index;       // Index of the sample in the audio data
};

class GrainProcessorImpl : public thl::dsp::BaseProcessor {
public:
    explicit GrainProcessorImpl(thl::dsp::audio::AudioDataStore& audio_store);
    ~GrainProcessorImpl() override;

    void prepare(const double& sample_rate, const size_t& samples_per_block, const size_t& num_channels) override;
    void process(float** buffer, const size_t& num_samples, const size_t& num_channels) override;

    void reset_grains();

    bool is_active() const { return m_envelope.is_active(); }

protected:
    enum Parameter {
        Playing = 0,
        Volume = 1,

        Size = 2,
        Density = 3,
        Velocity = 4,

        TemperatureSize = 5,
        TemperaturePosition = 6,
        TemperatureVelocity = 7,

        SampleIndex = 8,
        SampleStart = 9,
        SampleEnd = 10,
        SampleLoopPoint = 11,

        ChannelModeParam = 12,
        Spread = 13,

        EnvelopeAttack = 14,
        EnvelopeDecay = 15,
        EnvelopeSustain = 16,
        EnvelopeRelease = 17,

        NUM_PARAMETERS = 18
    };

    thl::dsp::utils::ADSR m_envelope;
    thl::dsp::audio::AudioDataStore& m_audio_store;

private:
    // Template wrapper for get_parameter
    template<typename T>
    T get_parameter(Parameter parameter);

    virtual float get_parameter_float(Parameter parameter) = 0;
    virtual bool get_parameter_bool(Parameter parameter) = 0;
    virtual int get_parameter_int(Parameter parameter) = 0;

    double m_sample_rate = 48000.0;
    size_t m_channels = 2;

    // Grain management
    std::vector<Grain> m_grains;
    size_t m_max_grains;
    size_t m_next_grain_time;
    size_t m_min_grain_interval;
    size_t m_sequential_position;

    // Random number generation for grain parameters
    std::mt19937 m_random_generator;
    std::uniform_real_distribution<float> m_uni_dist;

    // Envelope
    bool m_last_playing_state;
    bool m_is_first_grain{true};
    float m_last_envelope_attack{-1.0f};
    float m_last_envelope_decay{-1.0f};
    float m_last_envelope_sustain{-1.0f};
    float m_last_envelope_release{-1.0f};
    void update_envelope_if_needed();

    // Sample index management
    size_t m_current_sample_index;

    // Grain generation and management
    void trigger_grain(const size_t sample_index);
    void update_grains(float** buffer, size_t n_buffer_frames);
    void read_sample(float position, size_t sample_index, size_t source_channel, float& out_sample);
    size_t calculate_grain_size(float grain_size_param, float temperature);
    long calculate_start_position(size_t grain_size, long max_position, float temperature, size_t region_start, size_t loop_point);
    float calculate_velocity(float velocity, float temperature);
};

// Template specializations for get_parameter
template<> inline float GrainProcessorImpl::get_parameter<float>(Parameter p) { return get_parameter_float(p); }
template<> inline bool GrainProcessorImpl::get_parameter<bool>(Parameter p) { return get_parameter_bool(p); }
template<> inline int GrainProcessorImpl::get_parameter<int>(Parameter p) { return get_parameter_int(p); }

} // namespace thl::dsp::granular
