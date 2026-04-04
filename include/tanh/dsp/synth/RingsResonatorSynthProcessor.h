#pragma once

#include <cstdint>
#include <memory>
#include <tanh/core/Exports.h>
#include <tanh/dsp/audio/AudioBufferView.h>

namespace thl::dsp::synth {

enum class RingsPolyphonyMode { One, Two, Four };

class TANH_API RingsResonatorSynthProcessor {
public:
    enum Parameter {
        Frequency = 0,
        Structure,
        Brightness,
        Damping,
        Position,
        OddEvenMix,
        DryWet,
        Model,
        Polyphony,
        NumParameters
    };

    using PolyphonyMode = RingsPolyphonyMode;

    RingsResonatorSynthProcessor();
    virtual ~RingsResonatorSynthProcessor();

    RingsResonatorSynthProcessor(const RingsResonatorSynthProcessor&) = delete;
    RingsResonatorSynthProcessor& operator=(const RingsResonatorSynthProcessor&) = delete;
    RingsResonatorSynthProcessor(RingsResonatorSynthProcessor&&) noexcept;
    RingsResonatorSynthProcessor& operator=(RingsResonatorSynthProcessor&&) noexcept;

    void prepare(double sample_rate, int max_block_size);
    void process(const thl::dsp::audio::ConstAudioBufferView& input,
                 thl::dsp::audio::AudioBufferView output);
    int get_latency() const;

protected:
    virtual float get_parameter_float(Parameter parameter) = 0;
    virtual int get_parameter_int(Parameter parameter) = 0;

private:
    static constexpr size_t k_block_size = 24;

    struct EngineState;
    std::unique_ptr<EngineState> m_engine;
};

}  // namespace thl::dsp::synth
