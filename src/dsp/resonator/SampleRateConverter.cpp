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
    SRC_STATE* m_src_l = nullptr;
    SRC_STATE* m_src_r = nullptr;

    double m_source_rate = 48000.0;
    double m_target_rate = 48000.0;
    double m_ratio = 1.0;
    bool m_prepared = false;

    void prepare(double srcRate, double tgtRate, int /*maxBlockSize*/) {
        cleanup();

        m_prepared = true;
        m_source_rate = srcRate;
        m_target_rate = tgtRate;
        m_ratio = tgtRate / srcRate;

        int error = 0;
        m_src_l = src_new(SRC_SINC_FASTEST, 1, &error);
        m_src_r = src_new(SRC_SINC_FASTEST, 1, &error);
    }

    int process_mono(const float* input, int numInputSamples,
                     float* output, int maxOutputSamples) {
        SRC_DATA data{};
        data.data_in = input;
        data.input_frames = numInputSamples;
        data.data_out = output;
        data.output_frames = maxOutputSamples;
        data.src_ratio = m_ratio;
        data.end_of_input = 0;

        src_process(m_src_l, &data);

        return static_cast<int>(data.output_frames_gen);
    }

    int process_stereo(const float* inputL, const float* inputR,
                       int numInputSamples, float* outputL,
                       float* outputR, int maxOutputSamples) {
        SRC_DATA dataL{};
        dataL.data_in = inputL;
        dataL.input_frames = numInputSamples;
        dataL.data_out = outputL;
        dataL.output_frames = maxOutputSamples;
        dataL.src_ratio = m_ratio;
        dataL.end_of_input = 0;

        SRC_DATA dataR{};
        dataR.data_in = inputR;
        dataR.input_frames = numInputSamples;
        dataR.data_out = outputR;
        dataR.output_frames = maxOutputSamples;
        dataR.src_ratio = m_ratio;
        dataR.end_of_input = 0;

        src_process(m_src_l, &dataL);
        src_process(m_src_r, &dataR);

        return static_cast<int>(std::min(dataL.output_frames_gen, dataR.output_frames_gen));
    }

    void reset() {
        if (m_src_l) src_reset(m_src_l);
        if (m_src_r) src_reset(m_src_r);
        m_prepared = false;
    }

    void cleanup() {
        if (m_src_l) { src_delete(m_src_l); m_src_l = nullptr; }
        if (m_src_r) { src_delete(m_src_r); m_src_r = nullptr; }
    }

    ~Impl() {
        cleanup();
    }
};

SampleRateConverter::SampleRateConverter() : m_impl(std::make_unique<Impl>()) {}
SampleRateConverter::~SampleRateConverter() = default;
SampleRateConverter::SampleRateConverter(SampleRateConverter&&) noexcept = default;
SampleRateConverter& SampleRateConverter::operator=(SampleRateConverter&&) noexcept = default;

void SampleRateConverter::prepare(double sourceRate, double targetRate, int maxBlockSize) {
    m_impl->prepare(sourceRate, targetRate, maxBlockSize);
}

int SampleRateConverter::process_mono(const float* input, int numInputSamples,
                                      float* output, int maxOutputSamples) {
    return m_impl->process_mono(input, numInputSamples, output, maxOutputSamples);
}

int SampleRateConverter::process_stereo(const float* inputL, const float* inputR, int numInputSamples,
                                        float* outputL, float* outputR, int maxOutputSamples) {
    return m_impl->process_stereo(inputL, inputR, numInputSamples, outputL, outputR, maxOutputSamples);
}

double SampleRateConverter::get_ratio() const {
    return m_impl->m_ratio;
}

int SampleRateConverter::get_latency() const {
    return 0;
}

void SampleRateConverter::reset() {
    m_impl->reset();
}

// ============================================================================
// DspRateConverter Implementation
// ============================================================================

struct DspRateConverter::Impl {
    SampleRateConverter m_downsampler;
    SampleRateConverter m_upsampler;

    std::vector<float> m_downsample_buffer_l;
    std::vector<float> m_downsample_buffer_r;
    std::vector<float> m_upsample_buffer_l;
    std::vector<float> m_upsample_buffer_r;

    double m_host_rate = 48000.0;
    double m_dsp_rate = 32000.0;
    bool m_needs_conversion = true;

    void prepare(double hRate, double dRate, int maxBlockSize) {
        m_host_rate = hRate;
        m_dsp_rate = dRate;

        // Check if rates are close enough to skip resampling
        m_needs_conversion = std::abs(hRate - dRate) > 100.0;

        if (m_needs_conversion) {
            m_downsampler.prepare(hRate, dRate, maxBlockSize);
            m_upsampler.prepare(dRate, hRate, maxBlockSize);
        }

        // Allocate buffers with headroom
        int maxDownsampled = static_cast<int>(std::ceil(maxBlockSize * dRate / hRate)) + 64;
        int maxUpsampled = static_cast<int>(std::ceil(maxBlockSize * hRate / dRate)) + 64;

        m_downsample_buffer_l.resize(maxDownsampled);
        m_downsample_buffer_r.resize(maxDownsampled);
        m_upsample_buffer_l.resize(maxUpsampled);
        m_upsample_buffer_r.resize(maxUpsampled);
    }

    int downsample(const float* inputL, const float* inputR, int numInputSamples,
                   float* outputL, float* outputR, int maxOutputSamples) {
        return m_downsampler.process_stereo(inputL, inputR, numInputSamples,
                                            outputL, outputR, maxOutputSamples);
    }

    int upsample(const float* inputL, const float* inputR, int numInputSamples,
                 float* outputL, float* outputR, int maxOutputSamples) {
        return m_upsampler.process_stereo(inputL, inputR, numInputSamples,
                                          outputL, outputR, maxOutputSamples);
    }

    int get_total_latency() const {
        if (!m_needs_conversion) return 0;
        return m_downsampler.get_latency() + m_upsampler.get_latency();
    }

    void reset() {
        m_downsampler.reset();
        m_upsampler.reset();
    }
};

DspRateConverter::DspRateConverter() : m_impl(std::make_unique<Impl>()) {}
DspRateConverter::~DspRateConverter() = default;
DspRateConverter::DspRateConverter(DspRateConverter&&) noexcept = default;
DspRateConverter& DspRateConverter::operator=(DspRateConverter&&) noexcept = default;

void DspRateConverter::prepare(double hostRate, double dspRate, int maxBlockSize) {
    m_impl->prepare(hostRate, dspRate, maxBlockSize);
}

int DspRateConverter::downsample(const float* inputL, const float* inputR, int numInputSamples,
                                  float* outputL, float* outputR, int maxOutputSamples) {
    return m_impl->downsample(inputL, inputR, numInputSamples, outputL, outputR, maxOutputSamples);
}

int DspRateConverter::upsample(const float* inputL, const float* inputR, int numInputSamples,
                                float* outputL, float* outputR, int maxOutputSamples) {
    return m_impl->upsample(inputL, inputR, numInputSamples, outputL, outputR, maxOutputSamples);
}

int DspRateConverter::get_total_latency() const {
    return m_impl->get_total_latency();
}

bool DspRateConverter::needs_resampling() const {
    return m_impl->m_needs_conversion;
}

void DspRateConverter::reset() {
    m_impl->reset();
}

} // namespace thl::dsp::resonator
