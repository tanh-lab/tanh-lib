#include <tanh/core/BufferOperations.h>
#include <tanh/core/BufferView.h>
#include <tanh/dsp/utils/BandSplitter.h>
#include <tanh/dsp/utils/Limiter.h>
#include <tanh/dsp/utils/MultibandLimiter.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <cassert>
#include <cstddef>
#include <cstdint>

namespace thl::dsp::utils {

MultibandLimiterImpl::MultibandLimiterImpl(size_t num_bands, size_t order)
    : BandSplitter(num_bands, order) {
    // the limiters keep a reference to this, so the vector must never reallocate
    m_limiters.reserve(num_bands);
    for (size_t band = 0; band < num_bands; ++band) { m_limiters.emplace_back(*this, band); }
}

void MultibandLimiterImpl::prepare(const double& sample_rate,
                                   const size_t& samples_per_block,
                                   const size_t& num_channels) {
    BandSplitter::prepare(sample_rate, samples_per_block, num_channels);
    for (BandLimiter& limiter : m_limiters) {
        limiter.prepare(sample_rate, samples_per_block, num_channels);
    }
    m_band.resize(num_channels, samples_per_block);
}

void MultibandLimiterImpl::process(thl::core::BufferView buffer,
                                   uint32_t modulation_offset) TANH_NONBLOCKING_FUNCTION {
    process(buffer, modulation_offset, {});
}

void MultibandLimiterImpl::process(thl::core::BufferView buffer,
                                   uint32_t modulation_offset,
                                   thl::core::BufferView dry) TANH_NONBLOCKING_FUNCTION {
    const bool with_dry = dry.get_num_channels() > 0;
    assert(!with_dry || (dry.get_num_channels() == buffer.get_num_channels() &&
                         dry.get_num_samples() == buffer.get_num_samples()));
    const size_t num_samples = buffer.get_num_samples();

    process_band(buffer, 0);
    if (with_dry) { thl::core::BufferOperations::copy(dry, buffer); }
    m_limiters[0].process(buffer, modulation_offset);

    const thl::core::BufferView band = thl::core::BufferView(m_band).sub_block(0, num_samples);
    for (size_t band_index = 1; band_index < num_bands(); ++band_index) {
        process_band(band, band_index);
        if (with_dry) { thl::core::BufferOperations::add(dry, band); }
        m_limiters[band_index].process(band, modulation_offset);
        thl::core::BufferOperations::add(buffer, band);
    }
}

void MultibandLimiterImpl::reset() TANH_NONBLOCKING_FUNCTION {
    BandSplitter::reset();
    for (BandLimiter& limiter : m_limiters) { limiter.reset(); }
}

MultibandLimiterImpl::BandLimiter::BandLimiter(MultibandLimiterImpl& owner, size_t band)
    : m_owner(owner), m_band(band) {}

float MultibandLimiterImpl::BandLimiter::get_parameter_float(Parameter parameter,
                                                             uint32_t modulation_offset) {
    switch (parameter) {
        case LimiterImpl::Threshold:
            return m_owner.get_band_parameter_float(MultibandLimiterImpl::Threshold,
                                                    m_band,
                                                    modulation_offset);
        case LimiterImpl::Attack:
            return m_owner.get_band_parameter_float(MultibandLimiterImpl::Attack,
                                                    m_band,
                                                    modulation_offset);
        case LimiterImpl::Release:
            return m_owner.get_band_parameter_float(MultibandLimiterImpl::Release,
                                                    m_band,
                                                    modulation_offset);
        case LimiterImpl::NumParameters: break;
    }
    assert(false);
    return 0.0f;
}

}  // namespace thl::dsp::utils
