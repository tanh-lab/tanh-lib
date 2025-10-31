#ifndef SINEPROCESSOR_H
#define SINEPROCESSOR_H

#include <cmath>
#include <vector>

class SineProcessor {

    enum Parameters {
        FREQUENCY,
        AMPLITUDE
    };

public:
    SineProcessor();
    ~SineProcessor();
    
    // Process audio data
    void prepare(const float& sample_rate, const int& samples_per_block);
    void process(float* output_buffer, unsigned int n_buffer_frames);
    void set_parameter(const Parameters& param, float value);
    float get_parameter(const Parameters& param) const;

private:
    std::vector<float> m_phase;
    float m_frequency = 100.f; // Default frequency
    float m_amplitude = 0.5f;  // Default amplitude
    float m_sample_rate = 44100.f;
    int m_samples_per_block = 512;
};

#endif // SINEPROCESSOR_H