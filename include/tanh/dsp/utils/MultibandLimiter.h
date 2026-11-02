#pragma once

#include <tanh/core/AbiNamespace.h>
#include <tanh/core/Buffer.h>
#include <tanh/core/BufferView.h>
#include <tanh/core/Exports.h>
#include <tanh/dsp/utils/BandSplitter.h>
#include <tanh/dsp/utils/Limiter.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace thl::inline THL_ABI_NAMESPACE::dsp::utils {

/**
 * Splits the signal with BandSplitter, limits every band with its own limiter and sums the bands.
 * process_with_dry() also writes the sum of the bands before limiting: the input through the
 * crossovers' all-passes, so in phase with the output (a dry signal matches the phase only,
 * never the latency: BaseProcessor::process_with_dry()).
 *
 * A derived class implements get_band_parameter_float() to give every band its own threshold,
 * attack and release, and may implement get_crossover_parameter_float() (BandSplitter) for the
 * crossover frequencies, read at every process() call at @p modulation_offset; otherwise they
 * are set with set_frequency() or set_frequencies().
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
    thl::core::Buffer<float> m_band;
};

}  // namespace thl::inline THL_ABI_NAMESPACE::dsp::utils
