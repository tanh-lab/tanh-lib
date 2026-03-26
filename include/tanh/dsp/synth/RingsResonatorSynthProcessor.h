#pragma once

#include <cstdint>
#include <memory>
#include <tanh/dsp/audio/AudioBufferView.h>

namespace thl::dsp::synth {

enum class RingsParameter {
    Frequency,
    Structure,
    Brightness,
    Damping,
    Position,
    OddEvenMix,
    DryWet,
    Model,
    Polyphony,
    NUM_PARAMETERS
};

enum class RingsPolyphonyMode { One, Two, Four };

class RingsResonatorSynthProcessor {
public:
    using Parameter = RingsParameter;
    using PolyphonyMode = RingsPolyphonyMode;

    RingsResonatorSynthProcessor();
    virtual ~RingsResonatorSynthProcessor();

    RingsResonatorSynthProcessor(const RingsResonatorSynthProcessor&) = delete;
    RingsResonatorSynthProcessor& operator=(const RingsResonatorSynthProcessor&) = delete;
    RingsResonatorSynthProcessor(RingsResonatorSynthProcessor&&) noexcept;
    RingsResonatorSynthProcessor& operator=(RingsResonatorSynthProcessor&&) noexcept;

    void prepare(double sampleRate, int maxBlockSize);
    void process(thl::dsp::audio::ConstAudioBufferView input,
                 thl::dsp::audio::AudioBufferView output);
    int get_latency() const;

protected:
    virtual float get_parameter_value(RingsParameter parameter, uint32_t modulation_offset = 0) = 0;

private:
    static constexpr size_t kBlockSize = 24;

    struct EngineState;
    std::unique_ptr<EngineState> m_engine;
};

}  // namespace thl::dsp::synth
