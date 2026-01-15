#pragma once

#include <random>
#include <string>
#include <vector>
#include <tanh/tanh.h>

#include <tanh/dsp/utils/ADSR.h>
#include <tanh/dsp/utils/HannWindow.h>

namespace thl::dsp::granular
{

// Structure to represent a single grain
struct Grain {
    size_t start_position;     // Starting position in the sample
    size_t current_position;   // Current position within the grain
    size_t grain_size;         // Size of the grain in samples
    float velocity;            // Playback speed/velocity
    float amplitude;           // Grain amplitude/volume
    float gain;
    bool active;               // Whether the grain is currently active
    thl::dsp::utils::HannWindow envelope;       // Hann window envelope for amplitude modulation
    size_t sample_index;       // Index of the sample in the audio data
    size_t sample_pack_index;       // Index of the sample pack
};

class GrainProcessorImpl : public thl::dsp::BaseProcessor {
public:
    explicit GrainProcessorImpl(size_t grain_index);
    ~GrainProcessorImpl() override;

    void init();

    void prepare(const double& sample_rate, const size_t& samples_per_block, const size_t& num_channels) override;
    void process(float** buffer, const size_t& num_samples, const size_t& num_channels) override;


    void set_grain_index(size_t grain_index) { m_grain_index = grain_index; }

    // Process audio data
    void process(float* output_buffer, unsigned int n_buffer_frames);

    // Load a new WAV file
    bool prepare_audio_data();
    bool load_wav_file(const std::string& file_path, const size_t sample_pack_index, const size_t sample_index, const float gain);
    bool load_mp3_file(const std::string& file_path, const size_t sample_pack_index, const size_t sample_index, const float gain);
    bool load_mp3_from_memory(const char* data, int size, size_t sample_pack_index, size_t sample_index, float gain);
    bool load_all_samples();

protected:
    enum Parameter {
        GlobalEnvelopeAttack = 0,
        GlobalEnvelopeDecay = 1,
        GlobalEnvelopeSustain = 2,
        GlobalEnvelopeRelease = 3,
        Playing = 4,
        Volume = 5,
        Temperature = 6,
        Velocity = 7,
        Size = 8,
        Density = 9,
        SamplePackIndex = 10,
        SampleIndex = 11,
        SampleStart = 12,
        SampleEnd = 13,

        // Player Parameter (probably not inside group) TODO check
        KeyMode = 14,
        RootNote = 15,
        NUM_PARAMETERS = 16
    };

    inline static std::vector<std::vector<std::vector<float>>> m_audio_data = {};

private:
    double m_sample_rate = 48000.0;
    size_t m_channels = 2;

    virtual float getParameterFloat(Parameter parameter) = 0;
    virtual bool getParameterBool(Parameter parameter) = 0;
    virtual int getParameterInt(Parameter parameter) = 0;

    size_t m_grain_index;

    // Audio sample data
    size_t m_num_notes;

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
    utils::ADSR m_envelope;
    bool m_last_playing_state;

    // Note management
    size_t m_current_note;

    // Grain generation and management
    void trigger_grain(const size_t sample_index);
    void update_grains(float* output_buffer, unsigned int n_buffer_frames);
    void get_sample_with_interpolation(float position, float* samples, size_t sample_pack_index, size_t sample_index);

    std::vector<float> m_internal_buffer;
};

} // namespace thl::dsp::granular