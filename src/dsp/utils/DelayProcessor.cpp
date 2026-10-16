#include <tanh/core/BufferView.h>
#include <tanh/dsp/utils/DelayProcessor.h>
#include <tanh/dsp/utils/DynamicDelayLine.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace thl::dsp::utils {

DelayProcessor::DelayProcessor() = default;

DelayProcessor::DelayProcessor(size_t max_delay_samples) : m_max_delay_samples(max_delay_samples) {}

DelayProcessor::~DelayProcessor() = default;

DelayProcessor::DelayProcessor(DelayProcessor&&) noexcept = default;

DelayProcessor& DelayProcessor::operator=(DelayProcessor&&) noexcept = default;

void DelayProcessor::prepare(const double& /*sample_rate*/,
                             const size_t& /*samples_per_block*/,
                             const size_t& num_channels) {
    m_num_channels = num_channels;
    prepare_delay_lines();
}

void DelayProcessor::process(thl::core::BufferView buffer, uint32_t /*modulation_offset*/) {
    const size_t num_channels = std::min(buffer.get_num_channels(), m_delay_lines.size());
    const size_t num_samples = buffer.get_num_samples();

    const bool fading = m_previous_delay_samples != m_delay_samples;

    for (size_t ch = 0; ch < num_channels; ++ch) {
        DynamicDelayLine& line = m_delay_lines[ch];
        float* data = buffer.get_write_pointer(ch);
        for (size_t i = 0; i < num_samples; ++i) {
            const float input = data[i];
            // Read before writing, so the maximum delay still fits the line.
            const auto read = [&](size_t delay) { return delay == 0 ? input : line.read(delay); };
            const float current = read(m_delay_samples);
            const size_t position = m_crossfade_position + i;
            if (fading && position < m_crossfade_samples) {
                const float previous = read(m_previous_delay_samples);
                const float t =
                    static_cast<float>(position) / static_cast<float>(m_crossfade_samples);
                data[i] = previous + (current - previous) * t;
            } else {
                data[i] = current;
            }
            line.write(input);
        }
    }

    advance_crossfade(num_samples);
    m_has_processed = true;
}

void DelayProcessor::write(const thl::core::BufferView& buffer) {
    const size_t num_channels = std::min(buffer.get_num_channels(), m_delay_lines.size());
    const size_t num_samples = buffer.get_num_samples();

    for (size_t ch = 0; ch < num_channels; ++ch) {
        DynamicDelayLine& line = m_delay_lines[ch];
        const float* data = buffer.get_read_pointer(ch);
        for (size_t i = 0; i < num_samples; ++i) { line.write(data[i]); }
    }

    advance_crossfade(num_samples);
    m_has_processed = true;
}

void DelayProcessor::reset() {
    for (auto& line : m_delay_lines) { line.reset(); }
    m_previous_delay_samples = m_delay_samples;
    m_crossfade_position = 0;
    m_has_processed = false;
}

void DelayProcessor::set_max_delay_samples(size_t max_delay_samples) {
    m_max_delay_samples = max_delay_samples;
    m_delay_samples = std::min(m_delay_samples, m_max_delay_samples);
    if (m_prepared) {
        prepare_delay_lines();
    } else {
        reset();
    }
}

void DelayProcessor::set_delay_samples(size_t delay_samples) {
    const size_t delay = std::min(delay_samples, m_max_delay_samples);
    if (delay == m_delay_samples) { return; }

    const bool use_crossfade = m_has_processed && m_crossfade_samples > 0;
    m_previous_delay_samples = use_crossfade ? m_delay_samples : delay;
    m_delay_samples = delay;
    m_crossfade_position = 0;
}

void DelayProcessor::set_crossfade_samples(size_t crossfade_samples) {
    m_crossfade_samples = crossfade_samples;
    advance_crossfade(0);
}

void DelayProcessor::prepare_delay_lines() {
    m_delay_lines = std::vector<DynamicDelayLine>(m_max_delay_samples > 0 ? m_num_channels : 0);
    for (auto& line : m_delay_lines) { line.prepare(m_max_delay_samples); }
    m_prepared = true;
    reset();
}

void DelayProcessor::advance_crossfade(size_t num_samples) {
    if (m_previous_delay_samples == m_delay_samples) { return; }

    m_crossfade_position += num_samples;
    if (m_crossfade_position >= m_crossfade_samples) {
        m_previous_delay_samples = m_delay_samples;
        m_crossfade_position = 0;
    }
}

}  // namespace thl::dsp::utils
