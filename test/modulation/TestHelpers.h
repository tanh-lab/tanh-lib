#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <tanh/modulation/LFOSource.h>
#include <tanh/modulation/ModulationSource.h>
#include <tanh/state/ParameterDefinitions.h>

static constexpr double k_sample_rate = 48000.0;
static constexpr size_t k_block_size = 512;

// Helper: creates a modulatable float ParameterDefinition for modulation tests.
static thl::ParameterDefinition modulatable_float(float default_value = 0.0f) {
    return thl::ParameterDefinition::make_float("", thl::Range::linear(0.0f, 1.0f), default_value)
        .automatable(false)
        .modulatable(true);
}

// Concrete LFOSourceImpl for tests — provides parameter values directly.
class TestLFOSource : public thl::modulation::LFOSourceImpl {
public:
    float m_frequency = 1.0f;
    thl::modulation::LFOWaveform m_waveform = thl::modulation::LFOWaveform::Sine;
    int m_decimation = 1;

private:
    float get_parameter_float(Parameter p, uint32_t) override {
        switch (p) {
            case Frequency: return m_frequency;
            default: return 0.0f;
        }
    }
    int get_parameter_int(Parameter p, uint32_t) override {
        switch (p) {
            case Waveform: return static_cast<int>(m_waveform);
            case Decimation: return m_decimation;
            default: return 0;
        }
    }
};

// Poly-only test source — only provides per-voice output.
// Set m_voice_values before construction or use set_voice_values().
class PolyTestSource : public thl::modulation::ModulationSource {
public:
    std::vector<float> m_voice_values;

    explicit PolyTestSource(uint32_t num_voices = 0) : ModulationSource(false, num_voices, true) {}

    void prepare(double /*sample_rate*/, size_t samples_per_block) override {
        resize_buffers(samples_per_block);
    }

    void process_voice(uint32_t voice_index, size_t num_samples, size_t offset = 0) override {
        float* out = voice_output(voice_index);
        const float val = m_voice_values[voice_index];
        for (size_t i = offset; i < offset + num_samples; ++i) { out[i] = val; }
        if (num_samples > 0) {
            record_voice_change_point(voice_index, static_cast<uint32_t>(offset));
        }
    }
};

// Combined test source — provides both mono and per-voice output.
class CombinedTestSource : public thl::modulation::ModulationSource {
public:
    float m_mono_value = 0.0f;
    std::vector<float> m_voice_values;

    explicit CombinedTestSource(uint32_t num_voices = 0)
        : ModulationSource(true, num_voices, true) {}

    void prepare(double /*sample_rate*/, size_t samples_per_block) override {
        resize_buffers(samples_per_block);
    }

    void process(size_t num_samples, size_t offset = 0) override {
        for (size_t i = offset; i < offset + num_samples; ++i) {
            m_output_buffer[i] = m_mono_value;
        }
        m_last_output = m_mono_value;
    }

    void process_voice(uint32_t voice_index, size_t num_samples, size_t offset = 0) override {
        float* out = voice_output(voice_index);
        const float val = m_voice_values[voice_index];
        for (size_t i = offset; i < offset + num_samples; ++i) { out[i] = val; }
    }
};
