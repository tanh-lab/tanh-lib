#include <tanh/dsp/resonator/RingsResonator.h>
#include <tanh/dsp/resonator/RingBuffer.h>
#include <tanh/dsp/resonator/SampleRateConverter.h>
#include <tanh/dsp/resonator/ParamSmoother.h>

#include <tanh/dsp/resonator/rings/Part.h>
#include <tanh/dsp/resonator/rings/Strummer.h>
#include <tanh/dsp/resonator/rings/StringSynthPart.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace thl::dsp::resonator {

// Rings native sample rate
static constexpr double kRingsNativeRate = 48000.0;
static constexpr size_t kBlockSize = 24;

struct RingsResonator::Impl {
    rings::Part part;
    rings::StringSynthPart stringSynth;
    rings::Strummer strummer;

    uint16_t reverbBuffer[32768] = {};

    rings::Patch patch;
    rings::PerformanceState performanceState;

    float frequency = 440.0f;
    float oddEvenMix = 0.5f;
    float dryWet = 1.0f;
    ResonatorModel model = ResonatorModel::Modal;
    PolyphonyMode polyphony = PolyphonyMode::One;

    // Parameter smoothers
    ParamSmoother frequencySmoother;
    ParamSmoother structureSmoother;
    ParamSmoother brightnessSmoother;
    ParamSmoother dampingSmoother;
    ParamSmoother positionSmoother;
    ParamSmoother oddEvenSmoother;
    ParamSmoother dryWetSmoother;

    // Latency compensation
    double hostSampleRate = 48000.0;
    int latency = 0;
    RingBuffer dryDelayLine;

    // Sample rate conversion
    bool needsConversion = false;
    SampleRateConverter inputDownsampler;
    SampleRateConverter outputUpsamplerOdd;
    SampleRateConverter outputUpsamplerEven;

    // Buffers for downsampled/upsampled data
    std::vector<float> downsampledInput;
    std::vector<float> nativeOutputOdd;
    std::vector<float> nativeOutputEven;
    std::vector<float> upsampledOdd;
    std::vector<float> upsampledEven;

    // Input accumulation for fixed block processing
    RingBuffer inputFifo;

    // Output FIFOs
    RingBuffer outputFifoOdd;
    RingBuffer outputFifoEven;

    void init() {
        strummer.Init(0.01f, kRingsNativeRate / kBlockSize);
        part.Init(reverbBuffer);
        stringSynth.Init(reverbBuffer);

        patch.structure = 0.5f;
        patch.brightness = 0.5f;
        patch.damping = 0.5f;
        patch.position = 0.5f;

        performanceState.note = 48.0f;
        performanceState.tonic = 12.0f;
        performanceState.fm = 0.0f;
        performanceState.chord = 0;
        performanceState.internal_exciter = false;
        performanceState.internal_strum = false;
        performanceState.internal_note = false;
        performanceState.strum = false;
    }

    void prepare(double sampleRate, int maxBlockSize) {
        hostSampleRate = sampleRate;
        needsConversion = std::abs(sampleRate - kRingsNativeRate) > 100.0;

        if (needsConversion) {
            inputDownsampler.prepare(sampleRate, kRingsNativeRate, maxBlockSize);

            int maxNativeSamples = static_cast<int>(std::ceil(maxBlockSize * kRingsNativeRate / sampleRate)) + 64;
            outputUpsamplerOdd.prepare(kRingsNativeRate, sampleRate, maxNativeSamples);
            outputUpsamplerEven.prepare(kRingsNativeRate, sampleRate, maxNativeSamples);

            // Compute latency: downsampler + block accumulation + upsampler
            int dsLatency = inputDownsampler.getLatency(); // host samples
            int blockLatency = static_cast<int>(std::ceil(kBlockSize * sampleRate / kRingsNativeRate));
            int usLatencyNative = outputUpsamplerOdd.getLatency(); // native samples
            int usLatencyHost = static_cast<int>(std::ceil(usLatencyNative * sampleRate / kRingsNativeRate));
            latency = dsLatency + blockLatency + usLatencyHost;
        } else {
            latency = static_cast<int>(kBlockSize);
        }

        int maxDownsampled = static_cast<int>(std::ceil(maxBlockSize * kRingsNativeRate / sampleRate)) + 64;
        int maxUpsampled = maxBlockSize + 64;

        downsampledInput.resize(maxDownsampled);
        nativeOutputOdd.resize(maxDownsampled);
        nativeOutputEven.resize(maxDownsampled);
        upsampledOdd.resize(maxUpsampled);
        upsampledEven.resize(maxUpsampled);

        // Pre-allocate ring buffers so process() never allocates
        dryDelayLine.resize(latency + maxBlockSize + 16);
        inputFifo.resize(maxDownsampled + maxBlockSize);
        outputFifoOdd.resize(maxUpsampled + maxBlockSize);
        outputFifoEven.resize(maxUpsampled + maxBlockSize);

        constexpr float smoothTime = 0.05f; // 50ms
        frequencySmoother.prepare(sampleRate, smoothTime);
        structureSmoother.prepare(sampleRate, smoothTime);
        brightnessSmoother.prepare(sampleRate, smoothTime);
        dampingSmoother.prepare(sampleRate, smoothTime);
        positionSmoother.prepare(sampleRate, smoothTime);
        oddEvenSmoother.prepare(sampleRate, smoothTime);
        dryWetSmoother.prepare(sampleRate, smoothTime);
    }

    void renderBlock(const float* input, float* outOdd, float* outEven) {
        float midiNote = 12.0f * std::log2(frequency / 27.5f);
        performanceState.note = midiNote;

        static constexpr int polyVoices[] = { 1, 2, 4 };
        int poly = polyVoices[static_cast<int>(polyphony)];
        if (part.polyphony() != poly) {
            part.set_polyphony(poly);
            stringSynth.set_polyphony(poly);
        }
        part.set_model(static_cast<rings::ResonatorModel>(model));
        stringSynth.set_fx(static_cast<rings::FxType>(model));

        float in[kBlockSize];
        std::copy(input, input + kBlockSize, in);

        float out[kBlockSize] = {};
        float aux[kBlockSize] = {};

        strummer.Process(in, kBlockSize, &performanceState);

        if (model == ResonatorModel::StringAndReverb) {
            stringSynth.Process(performanceState, patch, in, out, aux, kBlockSize);
        } else {
            part.Process(performanceState, patch, in, out, aux, kBlockSize);
        }

        for (size_t i = 0; i < kBlockSize; ++i) {
            outOdd[i] = std::clamp(out[i], -1.0f, 1.0f);
            outEven[i] = std::clamp(aux[i], -1.0f, 1.0f);
        }
    }

    void process(const float* input, float* output, int numSamples) {
        // Smooth parameters
        frequencySmoother.setTarget(frequency);
        structureSmoother.setTarget(patch.structure);
        brightnessSmoother.setTarget(patch.brightness);
        dampingSmoother.setTarget(patch.damping);
        positionSmoother.setTarget(patch.position);
        oddEvenSmoother.setTarget(oddEvenMix);
        dryWetSmoother.setTarget(dryWet);

        float smoothedFrequency  = frequencySmoother.skip(numSamples);
        float smoothedStructure  = structureSmoother.skip(numSamples);
        float smoothedBrightness = brightnessSmoother.skip(numSamples);
        float smoothedDamping    = dampingSmoother.skip(numSamples);
        float smoothedPosition   = positionSmoother.skip(numSamples);
        float smoothedOddEven    = oddEvenSmoother.skip(numSamples);
        float smoothedDryWet     = dryWetSmoother.skip(numSamples);

        // Apply smoothed values
        frequency = smoothedFrequency;
        patch.structure = smoothedStructure;
        patch.brightness = smoothedBrightness;
        patch.damping = smoothedDamping;
        patch.position = smoothedPosition;
        oddEvenMix = smoothedOddEven;
        dryWet = smoothedDryWet;

        // Save dry input before processing (input/output may alias)
        for (int i = 0; i < numSamples; ++i) {
            dryDelayLine.push(input[i]);
        }

        // Process wet signal
        if (!needsConversion) {
            processNativeWet(input, output, numSamples);
        } else {
            processResampledWet(input, output, numSamples);
        }

        // Blend wet output with latency-compensated dry signal
        const float wetGain = dryWet;
        const float dryGain = 1.0f - dryWet;

        for (int i = 0; i < numSamples; ++i) {
            float dry = 0.0f;
            if (dryDelayLine.size() > latency) {
                dry = dryDelayLine.pop();
            }
            output[i] = output[i] * wetGain + dry * dryGain;
        }
    }

    void processResampledWet(const float* input, float* output, int numSamples) {
        // 1. Downsample input to native rate
        int downsampledCount = inputDownsampler.processMono(
            input, numSamples,
            downsampledInput.data(), static_cast<int>(downsampledInput.size())
        );

        // 2. Accumulate downsampled input
        for (int i = 0; i < downsampledCount; ++i) {
            inputFifo.push(downsampledInput[i]);
        }

        // 3. Process in fixed-size Rings blocks
        int processedNative = 0;
        while (static_cast<size_t>(inputFifo.size()) >= kBlockSize) {
            float blockInput[kBlockSize];
            for (size_t i = 0; i < kBlockSize; ++i) {
                blockInput[i] = inputFifo.pop();
            }
            renderBlock(blockInput,
                        nativeOutputOdd.data() + processedNative,
                        nativeOutputEven.data() + processedNative);
            processedNative += kBlockSize;
        }

        // 4. Upsample output to host rate
        if (processedNative > 0) {
            int upsampledOddCount = outputUpsamplerOdd.processMono(
                nativeOutputOdd.data(), processedNative,
                upsampledOdd.data(), static_cast<int>(upsampledOdd.size())
            );
            int upsampledEvenCount = outputUpsamplerEven.processMono(
                nativeOutputEven.data(), processedNative,
                upsampledEven.data(), static_cast<int>(upsampledEven.size())
            );

            for (int i = 0; i < upsampledOddCount; ++i) {
                outputFifoOdd.push(upsampledOdd[i]);
            }
            for (int i = 0; i < upsampledEvenCount; ++i) {
                outputFifoEven.push(upsampledEven[i]);
            }
        }

        // 5. Odd/even blend from FIFOs
        const float oddGain = 1.0f - oddEvenMix;
        const float evenGain = oddEvenMix;

        for (int i = 0; i < numSamples; ++i) {
            float odd = 0.0f, even = 0.0f;
            if (!outputFifoOdd.empty())
                odd = outputFifoOdd.pop();
            if (!outputFifoEven.empty())
                even = outputFifoEven.pop();
            output[i] = odd * oddGain + even * evenGain;
        }
    }

    void processNativeWet(const float* input, float* output, int numSamples) {
        // Accumulate input
        for (int i = 0; i < numSamples; ++i) {
            inputFifo.push(input[i]);
        }

        // Process complete blocks
        while (static_cast<size_t>(inputFifo.size()) >= kBlockSize) {
            float blockInput[kBlockSize];
            float blockOdd[kBlockSize];
            float blockEven[kBlockSize];

            for (size_t j = 0; j < kBlockSize; ++j) {
                blockInput[j] = inputFifo.pop();
            }

            renderBlock(blockInput, blockOdd, blockEven);

            for (size_t j = 0; j < kBlockSize; ++j) {
                outputFifoOdd.push(blockOdd[j]);
                outputFifoEven.push(blockEven[j]);
            }
        }

        // Odd/even blend from FIFOs
        const float oddGain = 1.0f - oddEvenMix;
        const float evenGain = oddEvenMix;

        for (int i = 0; i < numSamples; ++i) {
            float odd = 0.0f, even = 0.0f;
            if (!outputFifoOdd.empty())
                odd = outputFifoOdd.pop();
            if (!outputFifoEven.empty())
                even = outputFifoEven.pop();
            output[i] = odd * oddGain + even * evenGain;
        }
    }
};

RingsResonator::RingsResonator() : pImpl(std::make_unique<Impl>()) {
    pImpl->init();
}

RingsResonator::~RingsResonator() = default;
RingsResonator::RingsResonator(RingsResonator&&) noexcept = default;
RingsResonator& RingsResonator::operator=(RingsResonator&&) noexcept = default;

void RingsResonator::prepare(double sampleRate, int maxBlockSize) {
    pImpl->prepare(sampleRate, maxBlockSize);
}

void RingsResonator::process(const float* input, float* output, int numSamples) {
    pImpl->process(input, output, numSamples);
}

void RingsResonator::setFrequency(float frequency) {
    pImpl->frequency = std::clamp(frequency, 20.0f, 20000.0f);
}

void RingsResonator::setStructure(float structure) {
    pImpl->patch.structure = std::clamp(structure, 0.0f, 0.9995f);
}

void RingsResonator::setBrightness(float brightness) {
    pImpl->patch.brightness = std::clamp(brightness, 0.0f, 1.0f);
}

void RingsResonator::setDamping(float damping) {
    pImpl->patch.damping = std::clamp(damping, 0.0f, 0.9995f);
}

void RingsResonator::setPosition(float position) {
    pImpl->patch.position = std::clamp(position, 0.0f, 0.9995f);
}

void RingsResonator::setModel(ResonatorModel model) {
    pImpl->model = model;
}

void RingsResonator::setPolyphony(PolyphonyMode mode) {
    pImpl->polyphony = mode;
}

void RingsResonator::setOddEvenMix(float mix) {
    mix = std::clamp(mix, 0.0f, 1.0f);
    pImpl->oddEvenMix = 0.05f + mix * 0.9f;
}

void RingsResonator::setDryWet(float dryWet) {
    pImpl->dryWet = std::clamp(dryWet, 0.0f, 1.0f);
}

int RingsResonator::getLatency() const {
    return pImpl->latency;
}

} // namespace thl::dsp::resonator
