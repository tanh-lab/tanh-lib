#include <tanh/dsp/resonator/RingsResonator.h>
#include <tanh/dsp/resonator/RingBuffer.h>
#include <tanh/dsp/resonator/ParamSmoother.h>

#include <tanh/dsp/resonator/rings/Part.h>
#include <tanh/dsp/resonator/rings/Strummer.h>
#include <tanh/dsp/resonator/rings/StringSynthPart.h>
#include <tanh/dsp/resonator/rings/fx/Reverb.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace thl::dsp::resonator {

static constexpr size_t kBlockSize = 24;

struct RingsResonator::Impl {
    rings::Part m_part;
    rings::StringSynthPart m_string_synth;
    rings::Strummer m_strummer;

    uint16_t m_reverb_buffer[rings::Reverb::kReverbBufferSize] = {};

    rings::Patch m_patch;
    rings::PerformanceState m_performance_state;

    float m_frequency = 440.0f;
    float m_odd_even_mix = 0.5f;
    float m_dry_wet = 1.0f;
    ResonatorModel m_model = ResonatorModel::Modal;
    PolyphonyMode m_polyphony = PolyphonyMode::One;

    // Parameter smoothers
    ParamSmoother m_frequency_smoother;
    ParamSmoother m_structure_smoother;
    ParamSmoother m_brightness_smoother;
    ParamSmoother m_damping_smoother;
    ParamSmoother m_position_smoother;
    ParamSmoother m_odd_even_smoother;
    ParamSmoother m_dry_wet_smoother;

    // Latency compensation
    double m_host_sample_rate = 48000.0;
    int m_latency = static_cast<int>(kBlockSize);
    RingBuffer m_dry_delay_line;

    // Input accumulation for fixed block processing
    RingBuffer m_input_fifo;

    // Output FIFOs
    RingBuffer m_output_fifo_odd;
    RingBuffer m_output_fifo_even;

    void init() {
        m_patch.structure = 0.5f;
        m_patch.brightness = 0.5f;
        m_patch.damping = 0.5f;
        m_patch.position = 0.5f;

        m_performance_state.note = 48.0f;
        m_performance_state.tonic = 12.0f;
        m_performance_state.fm = 0.0f;
        m_performance_state.chord = 0;
        m_performance_state.internal_exciter = false;
        m_performance_state.internal_strum = false;
        m_performance_state.internal_note = false;
        m_performance_state.strum = false;
    }

    void prepare(double sampleRate, int maxBlockSize) {
        m_host_sample_rate = sampleRate;
        float sr = static_cast<float>(sampleRate);

        m_part.init(m_reverb_buffer, sr);
        m_string_synth.init(m_reverb_buffer, sr);
        m_strummer.init(0.01f, sr / kBlockSize, sr);

        m_latency = static_cast<int>(kBlockSize);

        // Pre-allocate ring buffers so process() never allocates
        m_dry_delay_line.resize(m_latency + maxBlockSize + 16);
        m_input_fifo.resize(maxBlockSize + kBlockSize);
        m_output_fifo_odd.resize(maxBlockSize + kBlockSize);
        m_output_fifo_even.resize(maxBlockSize + kBlockSize);

        // Clear FIFOs on prepare to avoid stale state
        m_dry_delay_line.clear();
        m_input_fifo.clear();
        m_output_fifo_odd.clear();
        m_output_fifo_even.clear();

        constexpr float smoothTime = 0.05f; // 50ms
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

        static constexpr int polyVoices[] = { 1, 2, 4 };
        int poly = polyVoices[static_cast<int>(m_polyphony)];
        if (m_part.polyphony() != poly) {
            m_part.set_polyphony(poly);
            m_string_synth.set_polyphony(poly);
        }
        m_part.set_model(static_cast<rings::ResonatorModel>(m_model));
        m_string_synth.set_fx(static_cast<rings::FxType>(m_model));

        float in[kBlockSize];
        std::copy(input, input + kBlockSize, in);

        float out[kBlockSize] = {};
        float aux[kBlockSize] = {};

        m_strummer.process(in, kBlockSize, &m_performance_state);

        if (m_model == ResonatorModel::StringAndReverb) {
            m_string_synth.process(m_performance_state, m_patch, in, out, aux, kBlockSize);
        } else {
            m_part.process(m_performance_state, m_patch, in, out, aux, kBlockSize);
        }

        for (size_t i = 0; i < kBlockSize; ++i) {
            outOdd[i] = std::clamp(out[i], -1.0f, 1.0f);
            outEven[i] = std::clamp(aux[i], -1.0f, 1.0f);
        }
    }

    void process(const float* input, float* output, int numSamples) {
        // Smooth parameters
        m_frequency_smoother.set_target(m_frequency);
        m_structure_smoother.set_target(m_patch.structure);
        m_brightness_smoother.set_target(m_patch.brightness);
        m_damping_smoother.set_target(m_patch.damping);
        m_position_smoother.set_target(m_patch.position);
        m_odd_even_smoother.set_target(m_odd_even_mix);
        m_dry_wet_smoother.set_target(m_dry_wet);

        float smoothedFrequency  = m_frequency_smoother.skip(numSamples);
        float smoothedStructure  = m_structure_smoother.skip(numSamples);
        float smoothedBrightness = m_brightness_smoother.skip(numSamples);
        float smoothedDamping    = m_damping_smoother.skip(numSamples);
        float smoothedPosition   = m_position_smoother.skip(numSamples);
        float smoothedOddEven    = m_odd_even_smoother.skip(numSamples);
        float smoothedDryWet     = m_dry_wet_smoother.skip(numSamples);

        // Apply smoothed values
        m_frequency = smoothedFrequency;
        m_patch.structure = smoothedStructure;
        m_patch.brightness = smoothedBrightness;
        m_patch.damping = smoothedDamping;
        m_patch.position = smoothedPosition;
        m_odd_even_mix = smoothedOddEven;
        m_dry_wet = smoothedDryWet;

        // Save dry input before processing (input/output may alias)
        for (int i = 0; i < numSamples; ++i) {
            m_dry_delay_line.push(input[i]);
        }

        // Accumulate input
        for (int i = 0; i < numSamples; ++i) {
            m_input_fifo.push(input[i]);
        }

        // Process complete blocks
        while (static_cast<size_t>(m_input_fifo.size()) >= kBlockSize) {
            float blockInput[kBlockSize];
            float blockOdd[kBlockSize];
            float blockEven[kBlockSize];

            for (size_t j = 0; j < kBlockSize; ++j) {
                blockInput[j] = m_input_fifo.pop();
            }

            render_block(blockInput, blockOdd, blockEven);

            for (size_t j = 0; j < kBlockSize; ++j) {
                m_output_fifo_odd.push(blockOdd[j]);
                m_output_fifo_even.push(blockEven[j]);
            }
        }

        // Odd/even blend from FIFOs
        const float oddGain = 1.0f - m_odd_even_mix;
        const float evenGain = m_odd_even_mix;

        for (int i = 0; i < numSamples; ++i) {
            float odd = 0.0f, even = 0.0f;
            if (!m_output_fifo_odd.empty())
                odd = m_output_fifo_odd.pop();
            if (!m_output_fifo_even.empty())
                even = m_output_fifo_even.pop();
            output[i] = odd * oddGain + even * evenGain;
        }

        // Blend wet output with latency-compensated dry signal
        const float wetGain = m_dry_wet;
        const float dryGain = 1.0f - m_dry_wet;

        for (int i = 0; i < numSamples; ++i) {
            float dry = 0.0f;
            if (m_dry_delay_line.size() > m_latency) {
                dry = m_dry_delay_line.pop();
            }
            output[i] = output[i] * wetGain + dry * dryGain;
        }
    }
};

RingsResonator::RingsResonator() : m_impl(std::make_unique<Impl>()) {
    m_impl->init();
}

RingsResonator::~RingsResonator() = default;
RingsResonator::RingsResonator(RingsResonator&&) noexcept = default;
RingsResonator& RingsResonator::operator=(RingsResonator&&) noexcept = default;

void RingsResonator::prepare(double sampleRate, int maxBlockSize) {
    m_impl->prepare(sampleRate, maxBlockSize);
}

void RingsResonator::process(const float* input, float* output, int numSamples) {
    m_impl->process(input, output, numSamples);
}

void RingsResonator::set_frequency(float frequency) {
    m_impl->m_frequency = std::clamp(frequency, 20.0f, 20000.0f);
}

void RingsResonator::set_structure(float structure) {
    m_impl->m_patch.structure = std::clamp(structure, 0.0f, 0.9995f);
}

void RingsResonator::set_brightness(float brightness) {
    m_impl->m_patch.brightness = std::clamp(brightness, 0.0f, 1.0f);
}

void RingsResonator::set_damping(float damping) {
    m_impl->m_patch.damping = std::clamp(damping, 0.0f, 0.9995f);
}

void RingsResonator::set_position(float position) {
    m_impl->m_patch.position = std::clamp(position, 0.0f, 0.9995f);
}

void RingsResonator::set_model(ResonatorModel model) {
    m_impl->m_model = model;
}

void RingsResonator::set_polyphony(PolyphonyMode mode) {
    m_impl->m_polyphony = mode;
}

void RingsResonator::set_odd_even_mix(float mix) {
    mix = std::clamp(mix, 0.0f, 1.0f);
    m_impl->m_odd_even_mix = 0.05f + mix * 0.9f;
}

void RingsResonator::set_dry_wet(float dryWet) {
    m_impl->m_dry_wet = std::clamp(dryWet, 0.0f, 1.0f);
}

int RingsResonator::get_latency() const {
    return m_impl->m_latency;
}

} // namespace thl::dsp::resonator
