#pragma once

#include <random>
#include <vector>
#include <tanh/tanh.h>
#include <tanh/dsp/audio/AudioDataStore.h>

namespace thl::dsp::granular {

// Structure to represent a single grain
struct Grain {
    size_t start_position;     // Starting position in the sample
    size_t current_position;   // Current position within the grain
    size_t grain_size;         // Size of the grain in samples
    float velocity;            // Playback speed/velocity
    float amplitude;           // Grain amplitude/volume
    float gain;
    bool active;               // Whether the grain is currently active
    utils::HannWindow envelope;       // Hann window envelope for amplitude modulation
    size_t sample_index;       // Index of the sample in the audio data
};

class GrainProcessorImpl : public thl::dsp::BaseProcessor {
public:
    explicit GrainProcessorImpl(size_t grain_index, audio::AudioDataStore& audio_store);
    ~GrainProcessorImpl() override;

    void prepare(const double& sample_rate, const size_t& samples_per_block, const size_t& num_channels) override;
    void process(float** buffer, const size_t& num_samples, const size_t& num_channels) override;

    void set_grain_index(size_t grain_index) { m_grain_index = grain_index; }

    // Process audio data
    void process(float* output_buffer, unsigned int n_buffer_frames);

protected:
    enum Parameter {
        EnvelopeAttack = 0,
        EnvelopeDecay = 1,
        EnvelopeSustain = 2,
        EnvelopeRelease = 3,

        Playing = 4,
        Volume = 5,
        Temperature = 6,
        Velocity = 7,
        Size = 8,
        Density = 9,

        SampleIndex = 10,
        SampleStart = 11,
        SampleEnd = 12,

        KeyMode = 13,
        RootNote = 14,

        NUM_PARAMETERS = 15
    };

    utils::ADSR m_envelope;
    audio::AudioDataStore& m_audio_store;

private:
    double m_sample_rate = 48000.0;
    size_t m_channels = 2;

    // Template wrapper for get_parameter
    template<typename T>
    T get_parameter(Parameter parameter);

    virtual float get_parameter_float(Parameter parameter) = 0;
    virtual bool get_parameter_bool(Parameter parameter) = 0;
    virtual int get_parameter_int(Parameter parameter) = 0;

    /**
     * Map a MIDI note number to a sample index.
     * Must be implemented by derived classes to define the mapping strategy.
     *
     * @param midi_note The MIDI note number
     * @return The sample index, or -1 if out of range
     */
    virtual int map_midi_note_to_sample_index(int midi_note) = 0;

    virtual void process_voice_fx(float* buffer, size_t num_samples, size_t num_channels, size_t voice_index, bool note_on) {}

    size_t m_grain_index;

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

    // Note management
    size_t m_current_note;

    // Grain generation and management
    void trigger_grain(const size_t sample_index);
    void update_grains(float* output_buffer, unsigned int n_buffer_frames);
    void get_sample_with_interpolation(float position, float* samples, size_t sample_index);

    std::vector<float> m_internal_buffer;
};

// Template specializations for get_parameter
template<> inline float GrainProcessorImpl::get_parameter<float>(Parameter p) { return get_parameter_float(p); }
template<> inline bool GrainProcessorImpl::get_parameter<bool>(Parameter p) { return get_parameter_bool(p); }
template<> inline int GrainProcessorImpl::get_parameter<int>(Parameter p) { return get_parameter_int(p); }

} // namespace thl::dsp::granular
