#pragma once

#include <tanh/core/Buffer.h>
#include <tanh/core/BufferView.h>
#include <tanh/core/Exports.h>
#include <tanh/dsp/utils/BandSplitter.h>
#include <tanh/dsp/utils/Limiter.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace thl::dsp::utils {

/**
 * Splits the signal with BandSplitter, limits every band with its own limiter and sums the bands.
 * process_with_dry() also writes the sum of the bands before limiting: the limiters add
 * no latency, so it is the matching dry signal.
 *
 * A derived class implements get_band_parameter_float() to give every band its own threshold,
 * attack and release, and may implement get_crossover_parameter_float() for the crossover
 * frequencies; otherwise they are set with set_frequency() or set_frequencies().
 */
class TANH_API MultibandLimiterImpl : public BandSplitter {
public:
    explicit MultibandLimiterImpl(size_t num_bands, size_t order);

    void prepare(const double& sample_rate,
                 const size_t& samples_per_block,
                 const size_t& num_channels) override;
    void process(thl::core::BufferView buffer,
                 uint32_t modulation_offset = 0) TANH_NONBLOCKING_FUNCTION override;
    /// Clears the crossovers and releases every band's limiter.
    void reset() TANH_NONBLOCKING_FUNCTION override;
    void process_with_dry(thl::core::BufferView buffer,
                          uint32_t modulation_offset,
                          thl::core::BufferView dry) TANH_NONBLOCKING_FUNCTION override;

protected:
    enum BandParameter {
        Threshold = 0,
        Attack = 1,
        Release = 2,

        NumBandParameters = 3
    };

    virtual float get_band_parameter_float(BandParameter parameter,
                                           size_t band,
                                           uint32_t modulation_offset = 0) = 0;

    /// The frequency (Hz) of crossover @p crossover (0: between bands 0 and 1), read for every
    /// crossover at the start of each process() call, at @p modulation_offset, and applied
    /// together (set_frequencies(): independent of order; crossing ones are sorted). Non-finite
    /// (the default): that crossover keeps what set_frequency() gave it.
    virtual float get_crossover_parameter_float(size_t crossover, uint32_t modulation_offset = 0);

private:
    class BandLimiter : public LimiterImpl {
    public:
        BandLimiter(MultibandLimiterImpl& owner, size_t band);

    protected:
        float get_parameter_float(Parameter parameter, uint32_t modulation_offset = 0) override;

    private:
        MultibandLimiterImpl& m_owner;
        size_t m_band;
    };

    std::vector<BandLimiter> m_limiters;
    std::vector<float> m_crossover_parameters;  // one per crossover, read every block
    thl::core::Buffer<float> m_band;
};

}  // namespace thl::dsp::utils
