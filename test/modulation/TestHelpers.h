#pragma once

#include <cstddef>
#include <cstdint>

#include <tanh/modulation/LFOSource.h>
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
