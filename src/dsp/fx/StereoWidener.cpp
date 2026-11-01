#include <tanh/core/AbiNamespace.h>
#include <tanh/core/BufferOperations.h>
#include <tanh/core/BufferView.h>
#include <tanh/dsp/fx/StereoWidener.h>
#include <tanh/dsp/utils/BandSplitter.h>
#include <tanh/dsp/utils/SmoothedValue.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace thl::inline THL_ABI_NAMESPACE::dsp::fx {

namespace {
constexpr size_t k_stereo = 2;
}  // namespace

StereoWidenerImpl::StereoWidenerImpl(size_t num_bands, size_t order)
    : BandSplitter(num_bands, order)
    , m_widths(num_bands)
    , m_width_targets(num_bands, 1.0f)
    , m_crossover_parameters(num_bands - 1, 0.0f) {}

float StereoWidenerImpl::get_crossover_parameter_float(size_t /*crossover*/,
                                                       uint32_t /*modulation_offset*/) {
    return std::numeric_limits<float>::quiet_NaN();
}

float StereoWidenerImpl::width_of(size_t band,
                                  uint32_t modulation_offset) TANH_NONBLOCKING_FUNCTION {
    const float width = get_band_parameter_float(Width, band, modulation_offset);
    return std::isfinite(width) ? std::clamp(width, 0.0f, k_max_width) : 1.0f;
}

void StereoWidenerImpl::read_parameters(uint32_t modulation_offset) TANH_NONBLOCKING_FUNCTION {
    for (size_t c = 0; c < m_crossover_parameters.size(); ++c) {
        m_crossover_parameters[c] = get_crossover_parameter_float(c, modulation_offset);
    }
    set_frequencies(m_crossover_parameters);
    for (size_t band = 0; band < m_widths.size(); ++band) {
        m_width_targets[band] = width_of(band, modulation_offset);
        m_widths[band].set_target_value(m_width_targets[band]);
    }
}

void StereoWidenerImpl::prepare(const double& sample_rate,
                                const size_t& samples_per_block,
                                const size_t& /*num_channels*/) {
    // Mid and side, whatever the host's channel count (only stereo is widened).
    BandSplitter::prepare(sample_rate, samples_per_block, k_stereo);
    m_mid_side.resize(k_stereo, samples_per_block);
    m_band.resize(k_stereo, samples_per_block);
    m_wide.resize(k_stereo, samples_per_block);
    m_through.resize(k_stereo, samples_per_block);
    for (auto& width : m_widths) { width.reset(sample_rate, k_ramp_seconds); }
    read_parameters(0);
    for (size_t band = 0; band < m_widths.size(); ++band) {
        m_widths[band].set_current_and_target_value(m_width_targets[band]);  // no glide at first
    }
}

void StereoWidenerImpl::reset() TANH_NONBLOCKING_FUNCTION {
    BandSplitter::reset();
    for (size_t band = 0; band < m_widths.size(); ++band) {
        m_widths[band].set_current_and_target_value(m_width_targets[band]);
    }
}

void StereoWidenerImpl::process(thl::core::BufferView buffer,
                                uint32_t modulation_offset) TANH_NONBLOCKING_FUNCTION {
    process_with_dry(buffer, modulation_offset, {});
}

void StereoWidenerImpl::process_with_dry(thl::core::BufferView buffer,
                                         uint32_t modulation_offset,
                                         thl::core::BufferView dry) TANH_NONBLOCKING_FUNCTION {
    const bool with_dry = dry.get_num_channels() > 0;
    assert(!with_dry || (dry.get_num_channels() == buffer.get_num_channels() &&
                         dry.get_num_samples() == buffer.get_num_samples()));
    if (buffer.get_num_channels() != k_stereo) {  // nothing to widen: passes unchanged
        if (with_dry) { thl::core::BufferOperations::copy(dry, buffer); }
        return;
    }
    const size_t n = buffer.get_num_samples();
    assert(n <= m_mid_side.get_num_samples());
    read_parameters(modulation_offset);

    // Encode: band 0 is split in place from it.
    thl::core::BufferView mid_side = thl::core::BufferView(m_mid_side).sub_block(0, n);
    {
        const float* left = buffer.get_read_pointer(0);
        const float* right = buffer.get_read_pointer(1);
        float* mid = mid_side.get_write_pointer(0);
        float* side = mid_side.get_write_pointer(1);
        for (size_t i = 0; i < n; ++i) {
            mid[i] = 0.5f * (left[i] + right[i]);
            side[i] = 0.5f * (left[i] - right[i]);
        }
    }

    float* wide_mid = m_wide.get_write_pointer(0);
    float* wide_side = m_wide.get_write_pointer(1);
    float* through_mid = m_through.get_write_pointer(0);
    float* through_side = m_through.get_write_pointer(1);
    const thl::core::BufferView band_view = thl::core::BufferView(m_band).sub_block(0, n);
    for (size_t band = 0; band < num_bands(); ++band) {
        const thl::core::BufferView& view = band == 0 ? mid_side : band_view;
        process_band(view, band);
        const float* mid = view.get_read_pointer(0);
        const float* side = view.get_read_pointer(1);
        auto& width = m_widths[band];
        for (size_t i = 0; i < n; ++i) {
            const float w = width.get_smoothed_value();
            if (band == 0) {
                wide_mid[i] = mid[i];
                wide_side[i] = w * side[i];
                through_mid[i] = mid[i];
                through_side[i] = side[i];
            } else {
                wide_mid[i] += mid[i];
                wide_side[i] += w * side[i];
                through_mid[i] += mid[i];
                through_side[i] += side[i];
            }
        }
    }

    // Decode.
    float* left = buffer.get_write_pointer(0);
    float* right = buffer.get_write_pointer(1);
    for (size_t i = 0; i < n; ++i) {
        left[i] = wide_mid[i] + wide_side[i];
        right[i] = wide_mid[i] - wide_side[i];
    }
    if (with_dry) {
        float* dry_left = dry.get_write_pointer(0);
        float* dry_right = dry.get_write_pointer(1);
        for (size_t i = 0; i < n; ++i) {
            dry_left[i] = through_mid[i] + through_side[i];
            dry_right[i] = through_mid[i] - through_side[i];
        }
    }
}

}  // namespace thl::inline THL_ABI_NAMESPACE::dsp::fx
