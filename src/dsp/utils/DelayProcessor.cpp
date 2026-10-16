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

    for (size_t ch = 0; ch < num_channels; ++ch) {
        DynamicDelayLine& line = m_delay_lines[ch];
        float* data = buffer.get_write_pointer(ch);
        for (size_t i = 0; i < num_samples; ++i) {
            const float input = data[i];
            // Read before writing, so the maximum delay still fits the line.
            data[i] = m_delay_samples == 0 ? input : line.read(m_delay_samples);
            line.write(input);
        }
    }
}

void DelayProcessor::write(const thl::core::BufferView& buffer) {
    const size_t num_channels = std::min(buffer.get_num_channels(), m_delay_lines.size());
    const size_t num_samples = buffer.get_num_samples();

    for (size_t ch = 0; ch < num_channels; ++ch) {
        DynamicDelayLine& line = m_delay_lines[ch];
        const float* data = buffer.get_read_pointer(ch);
        for (size_t i = 0; i < num_samples; ++i) { line.write(data[i]); }
    }
}

void DelayProcessor::reset() {
    for (auto& line : m_delay_lines) { line.reset(); }
}

void DelayProcessor::set_max_delay_samples(size_t max_delay_samples) {
    m_max_delay_samples = max_delay_samples;
    m_delay_samples = std::min(m_delay_samples, m_max_delay_samples);
    if (m_prepared) { prepare_delay_lines(); }
}

void DelayProcessor::set_delay_samples(size_t delay_samples) {
    m_delay_samples = std::min(delay_samples, m_max_delay_samples);
}

void DelayProcessor::prepare_delay_lines() {
    m_delay_lines = std::vector<DynamicDelayLine>(m_max_delay_samples > 0 ? m_num_channels : 0);
    for (auto& line : m_delay_lines) { line.prepare(m_max_delay_samples); }
    m_prepared = true;
}

}  // namespace thl::dsp::utils
