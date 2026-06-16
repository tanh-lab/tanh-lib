#pragma once

#include <tanh/core/Exports.h>
#include <tanh/modulation/ModulationSource.h>

#include <atomic>
#include <cstdint>

namespace thl::modulation {

enum class LFOWaveform {
    Sine = 0,
    Triangle = 1,
    Saw = 2,      // ramp up: -1 -> +1 across one cycle
    SawDown = 3,  // ramp down: +1 -> -1 across one cycle
    Square = 4,
    SampleAndHold = 5,
};

enum class LFOPolarity { Bipolar = 0, Unipolar = 1 };

class TANH_API LFOSourceImpl : public ModulationSource {
public:
    LFOSourceImpl() : ModulationSource(k_global_scope) {}
    ~LFOSourceImpl() override = default;

    void prepare(double sample_rate, size_t samples_per_block, uint32_t voice_count) override;
    void process(size_t num_samples, size_t offset = 0) override;

    // Reset the fade-in envelope to zero. Call when the LFO becomes active so
    // its output ramps in over `FadeIn` seconds.
    void reset_fade_in();

    // Lock-free getters for the UI/viz thread. Updated once per block at the
    // end of process(). Defaults are safe across the prepare() barrier.
    [[nodiscard]] float current_phase() const noexcept {
        return m_phase_atomic.load(std::memory_order_relaxed);
    }
    [[nodiscard]] float current_fade_in() const noexcept {
        return m_fade_in_atomic.load(std::memory_order_relaxed);
    }

protected:
    enum Parameter {
        Frequency = 0,
        Waveform = 1,
        Decimation = 2,
        PhaseOffset = 3,  // float 0..1, added to phase before sampling
        Bias = 4,         // float, DC offset added to output
        PulseWidth = 5,   // float 0..1, square duty cycle (0.5 = symmetric)
        Depth = 6,        // float 0..1, scales output amplitude
        Polarity = 7,     // int LFOPolarity (0 bipolar, 1 unipolar)
        Smooth = 8,       // float 0..1, slew time (0 = instant, 1 = ~1s tau)
        FadeIn = 9,       // float seconds, time for fade-in ramp 0 -> 1
        NumParameters = 10
    };

private:
    template <typename T>
    T get_parameter(Parameter parameter, uint32_t modulation_offset = 0);

    virtual float get_parameter_float(Parameter parameter, uint32_t modulation_offset = 0) = 0;
    virtual int get_parameter_int(Parameter parameter, uint32_t modulation_offset = 0) = 0;

    // Pure waveform sampler (no bias/depth/polarity/slew). Caller wraps the
    // phase argument into [0, 1). For SampleAndHold the value is read from
    // m_held_value rather than computed here.
    static float generate_sample_basic(float phase, LFOWaveform waveform, float pulse_width);

    // Uniform [-1, +1] sample from a fast xorshift PRNG. Used to latch a new
    // value on phase wrap for SampleAndHold.
    float next_random();

    double m_sample_rate = 48000.0;
    float m_phase = 0.0f;
    float m_phase_increment = 0.0f;
    uint32_t m_samples_until_update = 0;

    // S&H state — latched on phase wrap.
    float m_held_value = 0.0f;
    bool m_held_initialized = false;

    // Smoothing (one-pole slew) state.
    float m_smoothed_value = 0.0f;

    // Fade-in envelope state.
    float m_fade_in_value = 1.0f;

    // PRNG state for S&H.
    uint32_t m_prng_state = 0x12345678u;

    // Viz mirrors — written once per block at end of process().
    std::atomic<float> m_phase_atomic{0.0f};
    std::atomic<float> m_fade_in_atomic{1.0f};
};

template <>
inline float LFOSourceImpl::get_parameter<float>(Parameter p, uint32_t modulation_offset) {
    return get_parameter_float(p, modulation_offset);
}

template <>
inline int LFOSourceImpl::get_parameter<int>(Parameter p, uint32_t modulation_offset) {
    return get_parameter_int(p, modulation_offset);
}

}  // namespace thl::modulation
