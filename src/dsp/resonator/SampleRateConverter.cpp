#include <tanh/dsp/resonator/SampleRateConverter.h>
#include <samplerate.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace thl::dsp::resonator {

// ============================================================================
// SampleRateConverter Implementation
// ============================================================================

struct SampleRateConverter::Impl {
    SRC_STATE* srcL = nullptr;
    SRC_STATE* srcR = nullptr;

    double sourceRate = 48000.0;
    double targetRate = 48000.0;
    double ratio = 1.0;
    bool prepared = false;

    void prepare(double srcRate, double tgtRate, int /*maxBlockSize*/) {
        cleanup();

        prepared = true;
        sourceRate = srcRate;
        targetRate = tgtRate;
        ratio = tgtRate / srcRate;

        int error = 0;
        srcL = src_new(SRC_SINC_FASTEST, 1, &error);
        srcR = src_new(SRC_SINC_FASTEST, 1, &error);
    }

    int processMono(const float* input, int numInputSamples,
                    float* output, int maxOutputSamples) {
        SRC_DATA data{};
        data.data_in = input;
        data.input_frames = numInputSamples;
        data.data_out = output;
        data.output_frames = maxOutputSamples;
        data.src_ratio = ratio;
        data.end_of_input = 0;

        src_process(srcL, &data);

        return static_cast<int>(data.output_frames_gen);
    }

    int processStereo(const float* inputL, const float* inputR, int numInputSamples,
                      float* outputL, float* outputR, int maxOutputSamples) {
        SRC_DATA dataL{};
        dataL.data_in = inputL;
        dataL.input_frames = numInputSamples;
        dataL.data_out = outputL;
        dataL.output_frames = maxOutputSamples;
        dataL.src_ratio = ratio;
        dataL.end_of_input = 0;

        SRC_DATA dataR{};
        dataR.data_in = inputR;
        dataR.input_frames = numInputSamples;
        dataR.data_out = outputR;
        dataR.output_frames = maxOutputSamples;
        dataR.src_ratio = ratio;
        dataR.end_of_input = 0;

        src_process(srcL, &dataL);
        src_process(srcR, &dataR);

        return static_cast<int>(std::min(dataL.output_frames_gen, dataR.output_frames_gen));
    }

    void reset() {
        if (srcL) src_reset(srcL);
        if (srcR) src_reset(srcR);
        prepared = false;
    }

    void cleanup() {
        if (srcL) { src_delete(srcL); srcL = nullptr; }
        if (srcR) { src_delete(srcR); srcR = nullptr; }
    }

    ~Impl() {
        cleanup();
    }
};

SampleRateConverter::SampleRateConverter() : pImpl(std::make_unique<Impl>()) {}
SampleRateConverter::~SampleRateConverter() = default;
SampleRateConverter::SampleRateConverter(SampleRateConverter&&) noexcept = default;
SampleRateConverter& SampleRateConverter::operator=(SampleRateConverter&&) noexcept = default;

void SampleRateConverter::prepare(double sourceRate, double targetRate, int maxBlockSize) {
    pImpl->prepare(sourceRate, targetRate, maxBlockSize);
}

int SampleRateConverter::processMono(const float* input, int numInputSamples,
                                      float* output, int maxOutputSamples) {
    return pImpl->processMono(input, numInputSamples, output, maxOutputSamples);
}

int SampleRateConverter::processStereo(const float* inputL, const float* inputR, int numInputSamples,
                                        float* outputL, float* outputR, int maxOutputSamples) {
    return pImpl->processStereo(inputL, inputR, numInputSamples, outputL, outputR, maxOutputSamples);
}

double SampleRateConverter::getRatio() const {
    return pImpl->ratio;
}

int SampleRateConverter::getLatency() const {
    return 0;
}

void SampleRateConverter::reset() {
    pImpl->reset();
}

// ============================================================================
// DspRateConverter Implementation
// ============================================================================

struct DspRateConverter::Impl {
    SampleRateConverter downsampler;
    SampleRateConverter upsampler;

    std::vector<float> downsampleBufferL;
    std::vector<float> downsampleBufferR;
    std::vector<float> upsampleBufferL;
    std::vector<float> upsampleBufferR;

    double hostRate = 48000.0;
    double dspRate = 32000.0;
    bool needsConversion = true;

    void prepare(double hRate, double dRate, int maxBlockSize) {
        hostRate = hRate;
        dspRate = dRate;

        // Check if rates are close enough to skip resampling
        needsConversion = std::abs(hRate - dRate) > 100.0;

        if (needsConversion) {
            downsampler.prepare(hRate, dRate, maxBlockSize);
            upsampler.prepare(dRate, hRate, maxBlockSize);
        }

        // Allocate buffers with headroom
        int maxDownsampled = static_cast<int>(std::ceil(maxBlockSize * dRate / hRate)) + 64;
        int maxUpsampled = static_cast<int>(std::ceil(maxBlockSize * hRate / dRate)) + 64;

        downsampleBufferL.resize(maxDownsampled);
        downsampleBufferR.resize(maxDownsampled);
        upsampleBufferL.resize(maxUpsampled);
        upsampleBufferR.resize(maxUpsampled);
    }

    int downsample(const float* inputL, const float* inputR, int numInputSamples,
                   float* outputL, float* outputR, int maxOutputSamples) {
        return downsampler.processStereo(inputL, inputR, numInputSamples,
                                          outputL, outputR, maxOutputSamples);
    }

    int upsample(const float* inputL, const float* inputR, int numInputSamples,
                 float* outputL, float* outputR, int maxOutputSamples) {
        return upsampler.processStereo(inputL, inputR, numInputSamples,
                                        outputL, outputR, maxOutputSamples);
    }

    int getTotalLatency() const {
        if (!needsConversion) return 0;
        return downsampler.getLatency() + upsampler.getLatency();
    }

    void reset() {
        downsampler.reset();
        upsampler.reset();
    }
};

DspRateConverter::DspRateConverter() : pImpl(std::make_unique<Impl>()) {}
DspRateConverter::~DspRateConverter() = default;
DspRateConverter::DspRateConverter(DspRateConverter&&) noexcept = default;
DspRateConverter& DspRateConverter::operator=(DspRateConverter&&) noexcept = default;

void DspRateConverter::prepare(double hostRate, double dspRate, int maxBlockSize) {
    pImpl->prepare(hostRate, dspRate, maxBlockSize);
}

int DspRateConverter::downsample(const float* inputL, const float* inputR, int numInputSamples,
                                  float* outputL, float* outputR, int maxOutputSamples) {
    return pImpl->downsample(inputL, inputR, numInputSamples, outputL, outputR, maxOutputSamples);
}

int DspRateConverter::upsample(const float* inputL, const float* inputR, int numInputSamples,
                                float* outputL, float* outputR, int maxOutputSamples) {
    return pImpl->upsample(inputL, inputR, numInputSamples, outputL, outputR, maxOutputSamples);
}

int DspRateConverter::getTotalLatency() const {
    return pImpl->getTotalLatency();
}

bool DspRateConverter::needsResampling() const {
    return pImpl->needsConversion;
}

void DspRateConverter::reset() {
    pImpl->reset();
}

} // namespace thl::dsp::resonator
