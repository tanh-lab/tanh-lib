#include <tanh/core/AbiNamespace.h>
#include <tanh/core/BufferView.h>
#include <tanh/dsp/probe/SampleProbe.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>

namespace thl::inline THL_ABI_NAMESPACE::dsp::probe {

SampleProbe::SampleProbe(double capacity_seconds) : m_capacity_seconds(capacity_seconds) {}

void SampleProbe::prepare(double sample_rate, size_t max_block_size, size_t num_channels) {
    m_sample_rate = sample_rate;
    const auto seconds = static_cast<size_t>(std::ceil(m_capacity_seconds * sample_rate));
    m_ring.prepare(num_channels, std::max(seconds, 2 * max_block_size));
    m_channel_pointers.assign(num_channels, nullptr);
}

void SampleProbe::observe(const thl::core::BufferView& block) TANH_NONBLOCKING_FUNCTION {
    const size_t channels = std::min(block.get_num_channels(), m_channel_pointers.size());
    for (size_t ch = 0; ch < channels; ++ch) {
        m_channel_pointers[ch] = block.get_read_pointer(ch);
    }
    (void)m_ring.write(std::span<const float* const>(m_channel_pointers.data(), channels),
                       block.get_num_samples());
}

}  // namespace thl::inline THL_ABI_NAMESPACE::dsp::probe
