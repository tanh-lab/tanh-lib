#include <tanh/dsp/synth/RingsResonatorSynthProcessor.h>

#include <tanh/dsp/audio/AudioBufferView.h>
#include <tanh/dsp/audio/RingBuffer.h>
#include <tanh/dsp/utils/ParamSmoother.h>

#include <tanh/dsp/rings-resonator/RingsVoiceManager.h>
#include <tanh/dsp/rings-resonator/RingsStrummer.h>
#include <tanh/dsp/rings-resonator/RingsStringSynthPart.h>
#include <tanh/dsp/rings-resonator/fx/RingsReverb.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdint>

namespace thl::dsp::synth {

struct RingsResonatorSynthProcessor::EngineState {
    RingsVoiceManager m_part;
    RingsStringSynthPart m_string_synth;
    thl::dsp::resonator::RingsStrummer m_strummer;

    uint16_t m_reverb_buffer[thl::dsp::fx::RingsReverb::kReverbBufferSize] = {};

    thl::dsp::resonator::RingsPatch m_patch{0.5f, 0.5f, 0.5f, 0.5f};
    thl::dsp::resonator::RingsPerformanceState m_performance_state{
        false, false, false, false, 12.0f, 48.0f, 0.0f, 0};

    float m_frequency = 440.0f;
    float m_odd_even_mix = 0.5f;
    float m_dry_wet = 1.0f;
    resonator::ResonatorModel m_model = resonator::RESONATOR_MODEL_MODAL;
    int m_polyphony_voices = 1;

    utils::ParamSmoother m_frequency_smoother;
    utils::ParamSmoother m_structure_smoother;
    utils::ParamSmoother m_brightness_smoother;
    utils::ParamSmoother m_damping_smoother;
    utils::ParamSmoother m_position_smoother;
    utils::ParamSmoother m_odd_even_smoother;
    utils::ParamSmoother m_dry_wet_smoother;

    double m_host_sample_rate = 48000.0;
    int m_latency = static_cast<int>(kBlockSize);
    thl::dsp::audio::RingBuffer m_dry_delay_line;

    thl::dsp::audio::RingBuffer m_input_fifo;
    thl::dsp::audio::RingBuffer m_output_fifo_odd;
    thl::dsp::audio::RingBuffer m_output_fifo_even;

    void prepare(double sampleRate, int maxBlockSize) {
        m_host_sample_rate = sampleRate;
        float sr = static_cast<float>(sampleRate);

        m_part.prepare(m_reverb_buffer, sr);
        m_string_synth.prepare(m_reverb_buffer, sr);
        m_strummer.prepare(0.01f, sr / kBlockSize, sr);

        m_latency = static_cast<int>(kBlockSize);

        m_dry_delay_line.initialise_with_positions(1, m_latency + maxBlockSize + 16);
        m_input_fifo.initialise_with_positions(1, maxBlockSize + kBlockSize);
        m_output_fifo_odd.initialise_with_positions(1, maxBlockSize + kBlockSize);
        m_output_fifo_even.initialise_with_positions(1, maxBlockSize + kBlockSize);

        constexpr float smoothTime = 0.05f;
        m_frequency_smoother.prepare(sampleRate, smoothTime);
        m_structure_smoother.prepare(sampleRate, smoothTime);
        m_brightness_smoother.prepare(sampleRate, smoothTime);
        m_damping_smoother.prepare(sampleRate, smoothTime);
        m_position_smoother.prepare(sampleRate, smoothTime);
        m_odd_even_smoother.prepare(sampleRate, smoothTime);
        m_dry_wet_smoother.prepare(sampleRate, smoothTime);
    }

    void render_block(const float* input, float* outOdd, float* outEven) {
        float midiNote = 12.0f * std::log2(m_frequency / 27.5f);
        m_performance_state.note = midiNote;

        if (m_part.polyphony() != m_polyphony_voices) {
            m_part.set_polyphony(m_polyphony_voices);
            m_string_synth.set_polyphony(m_polyphony_voices);
        }
        m_part.set_model(m_model);
        m_string_synth.set_fx(static_cast<FxType>(static_cast<int>(m_model)));

        float in[kBlockSize];
        std::copy(input, input + kBlockSize, in);

        float out[kBlockSize] = {};
        float aux[kBlockSize] = {};

        thl::dsp::audio::ConstAudioBufferView in_view(in, kBlockSize);
        thl::dsp::audio::AudioBufferView out_view(out, kBlockSize);
        thl::dsp::audio::AudioBufferView aux_view(aux, kBlockSize);

        m_strummer.process(in_view, &m_performance_state);

        if (m_model == resonator::RESONATOR_MODEL_STRING_AND_REVERB) {
            m_string_synth.process(
                m_performance_state, m_patch, in_view, out_view, aux_view);
        } else {
            m_part.process(
                m_performance_state, m_patch, in_view, out_view, aux_view);
        }

        for (size_t i = 0; i < kBlockSize; ++i) {
            outOdd[i] = std::clamp(out[i], -1.0f, 1.0f);
            outEven[i] = std::clamp(aux[i], -1.0f, 1.0f);
        }
    }
};

RingsResonatorSynthProcessor::RingsResonatorSynthProcessor()
    : m_engine(std::make_unique<EngineState>()) {}

RingsResonatorSynthProcessor::~RingsResonatorSynthProcessor() = default;

RingsResonatorSynthProcessor::RingsResonatorSynthProcessor(
    RingsResonatorSynthProcessor&&) noexcept = default;

RingsResonatorSynthProcessor& RingsResonatorSynthProcessor::operator=(
    RingsResonatorSynthProcessor&&) noexcept = default;

void RingsResonatorSynthProcessor::prepare(double sampleRate, int maxBlockSize) {
    m_engine->prepare(sampleRate, maxBlockSize);
}

void RingsResonatorSynthProcessor::process(
    thl::dsp::audio::ConstAudioBufferView input,
    thl::dsp::audio::AudioBufferView output) {
    const float* input_ptr = input.get_read_pointer(0);
    float* output_ptr = output.get_write_pointer(0);
    int numSamples = static_cast<int>(input.get_num_frames());

    auto& e = *m_engine;

    float frequency = get_parameter_float(Parameter::Frequency);
    float structure = get_parameter_float(Parameter::Structure);
    float brightness = get_parameter_float(Parameter::Brightness);
    float damping = get_parameter_float(Parameter::Damping);
    float position = get_parameter_float(Parameter::Position);
    float oddEvenMix = get_parameter_float(Parameter::OddEvenMix);
    oddEvenMix = 0.05f + std::clamp(oddEvenMix, 0.0f, 1.0f) * 0.9f;
    float dryWet = get_parameter_float(Parameter::DryWet);
    int model = get_parameter_int(Parameter::Model);
    int polyphony = get_parameter_int(Parameter::Polyphony);

    e.m_frequency = frequency;
    e.m_patch.structure = structure;
    e.m_patch.brightness = brightness;
    e.m_patch.damping = damping;
    e.m_patch.position = position;
    e.m_odd_even_mix = oddEvenMix;
    e.m_dry_wet = dryWet;
    e.m_model = static_cast<resonator::ResonatorModel>(model);

    static constexpr int kPolyVoices[] = {1, 2, 4};
    int polyIndex = std::clamp(polyphony, 0, 2);
    e.m_polyphony_voices = kPolyVoices[polyIndex];

    e.m_frequency_smoother.set_target(e.m_frequency);
    e.m_structure_smoother.set_target(e.m_patch.structure);
    e.m_brightness_smoother.set_target(e.m_patch.brightness);
    e.m_damping_smoother.set_target(e.m_patch.damping);
    e.m_position_smoother.set_target(e.m_patch.position);
    e.m_odd_even_smoother.set_target(e.m_odd_even_mix);
    e.m_dry_wet_smoother.set_target(e.m_dry_wet);

    e.m_frequency = e.m_frequency_smoother.skip(numSamples);
    e.m_patch.structure = e.m_structure_smoother.skip(numSamples);
    e.m_patch.brightness = e.m_brightness_smoother.skip(numSamples);
    e.m_patch.damping = e.m_damping_smoother.skip(numSamples);
    e.m_patch.position = e.m_position_smoother.skip(numSamples);
    e.m_odd_even_mix = e.m_odd_even_smoother.skip(numSamples);
    e.m_dry_wet = e.m_dry_wet_smoother.skip(numSamples);

    for (int i = 0; i < numSamples; ++i) { e.m_dry_delay_line.push_sample(0, input_ptr[i]); }
    for (int i = 0; i < numSamples; ++i) { e.m_input_fifo.push_sample(0, input_ptr[i]); }

    while (e.m_input_fifo.get_available_samples(0) >= kBlockSize) {
        float blockInput[kBlockSize];
        float blockOdd[kBlockSize];
        float blockEven[kBlockSize];

        for (size_t j = 0; j < kBlockSize; ++j) {
            blockInput[j] = e.m_input_fifo.pop_sample(0);
        }

        e.render_block(blockInput, blockOdd, blockEven);

        for (size_t j = 0; j < kBlockSize; ++j) {
            e.m_output_fifo_odd.push_sample(0, blockOdd[j]);
            e.m_output_fifo_even.push_sample(0, blockEven[j]);
        }
    }

    const float oddGain = 1.0f - e.m_odd_even_mix;
    const float evenGain = e.m_odd_even_mix;

    for (int i = 0; i < numSamples; ++i) {
        float odd = 0.0f, even = 0.0f;
        if (e.m_output_fifo_odd.get_available_samples(0) > 0) odd = e.m_output_fifo_odd.pop_sample(0);
        if (e.m_output_fifo_even.get_available_samples(0) > 0) even = e.m_output_fifo_even.pop_sample(0);
        output_ptr[i] = odd * oddGain + even * evenGain;
    }

    const float wetGain = e.m_dry_wet;
    const float dryGain = 1.0f - e.m_dry_wet;

    for (int i = 0; i < numSamples; ++i) {
        float dry = 0.0f;
        if (static_cast<int>(e.m_dry_delay_line.get_available_samples(0)) > e.m_latency) {
            dry = e.m_dry_delay_line.pop_sample(0);
        }
        output_ptr[i] = output_ptr[i] * wetGain + dry * dryGain;
    }
}

int RingsResonatorSynthProcessor::get_latency() const {
    return m_engine->m_latency;
}

}  // namespace thl::dsp::synth
