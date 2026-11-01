#include <tanh/core/BufferView.h>
#include <tanh/dsp/filter/Butterworth.h>
#include <tanh/dsp/filter/LinkwitzRiley.h>
#include <tanh/dsp/utils/BandSplitter.h>
#include <tanh/dsp/utils/SmoothedValue.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <limits>
#include <span>
#include <vector>

namespace thl::dsp::utils {

BandSplitter::BandSplitter(size_t num_bands, size_t order)
    : m_num_bands(num_bands), m_order(order) {
    assert(num_bands >= 2);
    assert(order >= 2 && order <= filter::LinkwitzRiley::k_max_order && order % 2 == 0);
    const size_t num_crossovers = num_bands - 1;
    m_frequencies.resize(num_crossovers);
    m_smoothed_frequencies.resize(num_crossovers);
    for (size_t c = 0; c < num_crossovers; ++c) {
        // log-spaced from 100 Hz to 10 kHz, a single crossover in the middle at 1 kHz
        const float position = num_crossovers == 1
                                   ? 0.5f
                                   : static_cast<float>(c) / static_cast<float>(num_crossovers - 1);
        m_frequencies[c] = 100.0f * std::pow(100.0f, position);
    }
    m_requested = m_frequencies;
}

void BandSplitter::prepare(const double& sample_rate,
                           const size_t& samples_per_block,
                           const size_t& num_channels) {
    m_sample_rate = static_cast<float>(sample_rate);
    const size_t num_crossovers = m_num_bands - 1;
    apply_requests();  // the requests, for this sample rate
    // the filters can't be copied or moved, so the vectors are constructed with their size
    m_channels = std::vector<Channel>(num_channels);
    for (Channel& channel : m_channels) {
        channel = Channel(num_crossovers);
        for (size_t c = 0; c < num_crossovers; ++c) {
            Crossover& crossover = channel[c];
            crossover.m_filter.prepare(m_frequencies[c], m_sample_rate, m_order);
            crossover.m_band_all_passes = std::vector<filter::Butterworth>(c);
            for (filter::Butterworth& all_pass : crossover.m_band_all_passes) {
                all_pass.prepare(m_frequencies[c],
                                 m_sample_rate,
                                 m_order / 2,
                                 filter::Butterworth::Response::AllPass);
            }
        }
    }
    m_high_passed.resize(num_channels, samples_per_block);
    m_prewarped.resize(num_crossovers, samples_per_block);
    for (size_t c = 0; c < num_crossovers; ++c) {
        m_smoothed_frequencies[c].reset(sample_rate, k_smoothing_seconds);
        m_smoothed_frequencies[c].set_current_and_target_value(m_frequencies[c]);
    }
    m_next_band = 0;
}

void BandSplitter::set_frequency(size_t crossover, float frequency) TANH_NONBLOCKING_FUNCTION {
    assert(crossover < m_requested.size());
    if (!std::isfinite(frequency)) { return; }
    // Blocked at the neighbours' requests; Nyquist is applied from the request in apply_requests(),
    // so a request above it comes back at a higher sample rate.
    const float lowest = crossover > 0 ? m_requested[crossover - 1] : 0.0f;
    const float highest = crossover + 1 < m_requested.size()
                              ? m_requested[crossover + 1]
                              : std::numeric_limits<float>::infinity();
    m_requested[crossover] = std::clamp(frequency, lowest, highest);
    apply_requests();
}

void BandSplitter::set_frequencies(std::span<const float> frequencies) TANH_NONBLOCKING_FUNCTION {
    assert(frequencies.size() == m_requested.size());
    for (size_t c = 0; c < m_requested.size() && c < frequencies.size(); ++c) {
        if (std::isfinite(frequencies[c])) { m_requested[c] = std::max(frequencies[c], 0.0f); }
    }
    std::ranges::sort(m_requested);  // in place, no allocation
    apply_requests();
}

void BandSplitter::apply_requests() TANH_NONBLOCKING_FUNCTION {
    const float nyquist_limit = filter::Butterworth::k_max_frequency_ratio * m_sample_rate;
    for (size_t c = 0; c < m_frequencies.size(); ++c) {
        const float lowest = c > 0 ? m_frequencies[c - 1] : 0.0f;
        const float applied = std::max(std::min(m_requested[c], nyquist_limit), lowest);
        if (applied != m_frequencies[c]) {
            m_frequencies[c] = applied;
            m_smoothed_frequencies[c].set_target_value(applied);
        }
    }
}

float BandSplitter::get_frequency(size_t crossover) const {
    assert(crossover < m_frequencies.size());
    return m_frequencies[crossover];
}

void BandSplitter::reset() TANH_NONBLOCKING_FUNCTION {
    for (Channel& channel : m_channels) {
        for (Crossover& crossover : channel) {
            crossover.m_filter.reset();
            for (filter::Butterworth& all_pass : crossover.m_band_all_passes) { all_pass.reset(); }
        }
    }
    for (size_t c = 0; c < m_smoothed_frequencies.size(); ++c) {
        m_smoothed_frequencies[c].set_current_and_target_value(m_frequencies[c]);
    }
    m_next_band = 0;
}

void BandSplitter::process_band(thl::core::BufferView buffer,
                                size_t band_index) TANH_NONBLOCKING_FUNCTION {
    assert(band_index == m_next_band);
    assert(buffer.get_num_channels() == m_channels.size());
    const size_t num_crossovers = m_num_bands - 1;
    const size_t num_samples = buffer.get_num_samples();

    if (band_index == 0) {
        assert(num_samples <= m_high_passed.get_num_samples());
        m_num_samples = num_samples;
        update_prewarped(num_samples);
    } else {
        assert(num_samples == m_num_samples);
    }

    for (size_t channel_index = 0; channel_index < m_channels.size(); ++channel_index) {
        float* band = buffer.get_write_pointer(channel_index);
        float* high_passed = m_high_passed.get_write_pointer(channel_index);

        if (band_index == num_crossovers) {
            std::copy(high_passed, high_passed + num_samples, band);
            continue;
        }

        Channel& channel = m_channels[channel_index];
        filter::LinkwitzRiley& crossover = channel[band_index].m_filter;
        const float* input = band_index == 0 ? band : high_passed;
        const float* g = m_prewarped.get_read_pointer(band_index);
        for (size_t i = 0; i < num_samples; ++i) {
            const float x = input[i];
            float sample = crossover.process_low(x, g[i]);
            high_passed[i] = crossover.process_high(x, g[i]);
            for (size_t c = band_index + 1; c < num_crossovers; ++c) {
                sample = channel[c].m_band_all_passes[band_index].process(
                    sample,
                    m_prewarped.get_read_pointer(c)[i]);
            }
            band[i] = sample;
        }
    }

    m_next_band = (band_index + 1) % m_num_bands;
}

void BandSplitter::update_prewarped(size_t num_samples) TANH_NONBLOCKING_FUNCTION {
    for (size_t c = 0; c < m_smoothed_frequencies.size(); ++c) {
        SmoothedValue& smoothed_frequency = m_smoothed_frequencies[c];
        float* prewarped = m_prewarped.get_write_pointer(c);
        if (smoothed_frequency.is_smoothing()) {
            for (size_t i = 0; i < num_samples; ++i) {
                prewarped[i] = filter::Butterworth::prewarp(smoothed_frequency.get_smoothed_value(),
                                                            m_sample_rate);
            }
        } else {
            std::fill(prewarped,
                      prewarped + num_samples,
                      filter::Butterworth::prewarp(m_frequencies[c], m_sample_rate));
        }
    }
}

}  // namespace thl::dsp::utils
