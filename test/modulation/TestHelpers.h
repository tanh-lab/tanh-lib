#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <tanh/modulation/LFOSource.h>
#include <tanh/modulation/ModulationSource.h>
#include <tanh/modulation/ResolvedTarget.h>
#include <tanh/state/ModulationScope.h>
#include <tanh/state/ParameterDefinitions.h>

// Test-only accessors for the mono/voice buffer pointers on a ResolvedTarget.
// The production RT path loads these atomically with acquire semantics under
// a ReadScope; tests run single-threaded after matrix.process() and just
// need a plain load for assertions.
inline const thl::modulation::MonoBuffers* mono_of(const thl::modulation::ResolvedTarget* t) {
    return t == nullptr ? nullptr : t->m_mono.load(std::memory_order_relaxed);
}
inline const thl::modulation::VoiceBuffers* voice_of(const thl::modulation::ResolvedTarget* t) {
    return t == nullptr ? nullptr : t->m_voice.load(std::memory_order_relaxed);
}

// Semantic flags replacing the old ResolvedTarget::m_has_mono_* booleans.
inline bool has_mono_additive(const thl::modulation::ResolvedTarget* t) {
    auto* m = mono_of(t);
    return m != nullptr && m->m_has_additive;
}
inline bool has_mono_replace(const thl::modulation::ResolvedTarget* t) {
    auto* m = mono_of(t);
    return m != nullptr && m->m_has_replace;
}

static constexpr double k_sample_rate = 48000.0;
static constexpr size_t k_block_size = 512;

// Helper: creates a modulatable float ParameterDefinition for modulation tests.
static thl::ParameterDefinition modulatable_float(float default_value = 0.0f) {
    return thl::ParameterDefinition::make_float("", thl::Range::linear(0.0f, 1.0f), default_value)
        .automatable(false)
        .modulatable(true);
}

// Overload that tags the parameter with a modulation scope — required when the
// test registers a scope on the matrix and declares per-voice params.
static thl::ParameterDefinition modulatable_float(float default_value, thl::modulation::ModulationScope scope) {
    return thl::ParameterDefinition::make_float("", thl::Range::linear(0.0f, 1.0f), default_value)
        .automatable(false)
        .modulatable(true)
        .modulation_scope(scope);
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

    explicit PolyTestSource(thl::modulation::ModulationScope scope) : ModulationSource(scope, true) {}

    void prepare(double /*sample_rate*/, size_t samples_per_block, uint32_t voice_count) override {
        resize_buffers(samples_per_block, voice_count);
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
// Post-refactor sources are strict single-scope, so this shim keeps the mono
// slot for compatibility but returns the mono value via process_voice as well.
class CombinedTestSource : public thl::modulation::ModulationSource {
public:
    float m_mono_value = 0.0f;
    std::vector<float> m_voice_values;

    explicit CombinedTestSource(thl::modulation::ModulationScope scope) : ModulationSource(scope, true) {}

    void prepare(double /*sample_rate*/, size_t samples_per_block, uint32_t voice_count) override {
        resize_buffers(samples_per_block, voice_count);
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
