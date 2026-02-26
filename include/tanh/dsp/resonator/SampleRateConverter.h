#pragma once

#include <cstdint>
#include <memory>

namespace thl::dsp::resonator {

/**
 * @brief Low-latency sample rate converter using libsamplerate
 *
 * Handles streaming resampling with proper stereo handling.
 * Uses SRC_SINC_FASTEST for minimal latency.
 */
class SampleRateConverter {
public:
    SampleRateConverter();
    ~SampleRateConverter();

    // Non-copyable, movable
    SampleRateConverter(const SampleRateConverter&) = delete;
    SampleRateConverter& operator=(const SampleRateConverter&) = delete;
    SampleRateConverter(SampleRateConverter&&) noexcept;
    SampleRateConverter& operator=(SampleRateConverter&&) noexcept;

    /**
     * @brief Prepare the converter for a given source and target rate
     *
     * @param sourceRate Input sample rate in Hz
     * @param targetRate Output sample rate in Hz
     * @param maxBlockSize Maximum number of samples per process call
     */
    void prepare(double sourceRate, double targetRate, int maxBlockSize);

    /**
     * @brief Process mono audio
     *
     * @param input Input samples at source rate
     * @param numInputSamples Number of input samples
     * @param output Output buffer (must be large enough)
     * @param maxOutputSamples Maximum output buffer size
     * @return Number of output samples produced
     */
    int process_mono(const float* input, int numInputSamples,
                     float* output, int maxOutputSamples);

    /**
     * @brief Process stereo audio
     *
     * @param inputL Left input samples at source rate
     * @param inputR Right input samples at source rate
     * @param numInputSamples Number of input samples
     * @param outputL Left output buffer
     * @param outputR Right output buffer
     * @param maxOutputSamples Maximum output buffer size
     * @return Number of output samples produced (same for L and R)
     */
    int process_stereo(const float* inputL, const float* inputR,
                       int numInputSamples, float* outputL,
                       float* outputR, int maxOutputSamples);

    /**
     * @brief Get the conversion ratio (target/source)
     */
    double get_ratio() const;

    /**
     * @brief Get the latency in output samples
     */
    int get_latency() const;

    /**
     * @brief Reset the converter state
     */
    void reset();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/**
 * @brief Bidirectional sample rate converter for DSP processing
 *
 * Convenience class that manages both down-sampling to a native DSP rate
 * and up-sampling back to the host rate.
 */
class DspRateConverter {
public:
    DspRateConverter();
    ~DspRateConverter();

    // Non-copyable, movable
    DspRateConverter(const DspRateConverter&) = delete;
    DspRateConverter& operator=(const DspRateConverter&) = delete;
    DspRateConverter(DspRateConverter&&) noexcept;
    DspRateConverter& operator=(DspRateConverter&&) noexcept;

    /**
     * @brief Prepare the converter
     *
     * @param hostRate Host/DAW sample rate in Hz
     * @param dspRate Native DSP processing rate in Hz
     * @param maxBlockSize Maximum number of samples per process call
     */
    void prepare(double hostRate, double dspRate, int maxBlockSize);

    /**
     * @brief Downsample from host rate to DSP rate (stereo)
     *
     * @param inputL Left input at host rate
     * @param inputR Right input at host rate
     * @param numInputSamples Number of input samples
     * @param outputL Left output at DSP rate
     * @param outputR Right output at DSP rate
     * @param maxOutputSamples Maximum output buffer size
     * @return Number of output samples at DSP rate
     */
    int downsample(const float* inputL, const float* inputR,
                   int numInputSamples, float* outputL, float* outputR,
                   int maxOutputSamples);

    /**
     * @brief Upsample from DSP rate back to host rate (stereo)
     *
     * @param inputL Left input at DSP rate
     * @param inputR Right input at DSP rate
     * @param numInputSamples Number of input samples
     * @param outputL Left output at host rate
     * @param outputR Right output at host rate
     * @param maxOutputSamples Maximum output buffer size
     * @return Number of output samples at host rate
     */
    int upsample(const float* inputL, const float* inputR,
                 int numInputSamples, float* outputL, float* outputR,
                 int maxOutputSamples);

    /**
     * @brief Get the total round-trip latency in host samples
     */
    int get_total_latency() const;

    /**
     * @brief Check if resampling is needed (rates differ significantly)
     */
    bool needs_resampling() const;

    /**
     * @brief Reset converter state
     */
    void reset();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace thl::dsp::resonator
