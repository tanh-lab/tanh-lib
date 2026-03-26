#pragma once

#include <cstdint>
#include <tanh/modulation/ModulationSource.h>

namespace thl::modulation {

enum class LFOWaveform {
    Sine = 0,
    Triangle,
    Saw,
    Square
};

class LFOSourceImpl : public ModulationSource {
public:
    LFOSourceImpl() = default;
    ~LFOSourceImpl() override = default;

    void prepare(double sample_rate, size_t samples_per_block) override;
    void process(size_t num_samples) override;
    void process_single(float* out, uint32_t sample_index) override;

protected:
    enum Parameter {
        Frequency = 0,
        Waveform = 1,
        Decimation = 2,

        NUM_PARAMETERS = 3
    };

private:
    template <typename T>
    T get_parameter(Parameter parameter, uint32_t modulation_offset = 0);

    virtual float get_parameter_float(Parameter parameter, uint32_t modulation_offset = 0) = 0;
    virtual int get_parameter_int(Parameter parameter, uint32_t modulation_offset = 0) = 0;

    static float generate_sample(float phase, LFOWaveform waveform);

    double m_sample_rate = 48000.0;
    float m_phase = 0.0f;
    float m_phase_increment = 0.0f;

    uint32_t m_samples_until_update = 0;
};

template <>
inline float LFOSourceImpl::get_parameter<float>(Parameter p,
                                                  uint32_t modulation_offset) {
    return get_parameter_float(p, modulation_offset);
}

template <>
inline int LFOSourceImpl::get_parameter<int>(Parameter p,
                                              uint32_t modulation_offset) {
    return get_parameter_int(p, modulation_offset);
}

}  // namespace thl::modulation
