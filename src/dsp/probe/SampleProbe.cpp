#include <tanh/core/AbiNamespace.h>
#include <tanh/core/BufferView.h>
#include <tanh/dsp/probe/SampleProbe.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace thl::inline THL_ABI_NAMESPACE::dsp::probe {

SampleProbe::SampleProbe(double capacity_seconds) : m_capacity_seconds(capacity_seconds) {}

void SampleProbe::prepare(double sample_rate, size_t max_block_size, size_t num_channels) {
    m_sample_rate.store(sample_rate, std::memory_order_relaxed);
    const auto seconds = static_cast<size_t>(std::ceil(m_capacity_seconds * sample_rate));
    m_ring.prepare(num_channels, std::max(seconds, 2 * max_block_size));
    m_channel_pointers.assign(num_channels, nullptr);
    m_written = 0;
    m_read = 0;
    m_seen_generation = 0;
    m_discontinuity.store(0, std::memory_order_relaxed);
}

void SampleProbe::mark_discontinuity() TANH_NONBLOCKING_FUNCTION {
    const uint64_t generation =
        (m_discontinuity.load(std::memory_order_relaxed) >> k_position_bits) + 1;
    m_discontinuity.store((generation << k_position_bits) | (m_written & k_position_mask),
                          std::memory_order_release);
}

void SampleProbe::reset() TANH_NONBLOCKING_FUNCTION {
    mark_discontinuity();
}

void SampleProbe::observe(const thl::core::BufferView& block) TANH_NONBLOCKING_FUNCTION {
    const size_t channels = std::min(block.get_num_channels(), m_channel_pointers.size());
    for (size_t ch = 0; ch < channels; ++ch) {
        m_channel_pointers[ch] = block.get_read_pointer(ch);
    }
    const size_t offered = block.get_num_samples();
    const size_t written =
        m_ring.write(std::span<const float* const>(m_channel_pointers.data(), channels), offered);
    m_written += written;
    // The reader fell behind: what follows does not continue what it will read.
    if (written < offered) { mark_discontinuity(); }
}

SampleProbe::ReadResult SampleProbe::read(std::span<float* const> channels,
                                          size_t num_samples) TANH_NONBLOCKING_FUNCTION {
    // The writer marks a gap before it publishes what follows it: a mark not yet seen when the
    // samples were counted is seen after, and the read stops at it.
    ReadResult result;
    for (int attempt = 0; attempt < k_max_read_attempts; ++attempt) {
        // Skip to the newest mark not acted on yet: everything before it is from before a gap.
        const uint64_t mark = m_discontinuity.load(std::memory_order_acquire);
        if ((mark >> k_position_bits) != m_seen_generation) {
            m_seen_generation = mark >> k_position_bits;
            const uint64_t position = mark & k_position_mask;
            const uint64_t read_position = m_read & k_position_mask;
            if (position > read_position) { m_read += m_ring.skip(position - read_position); }
            result.m_discontinuity = true;
        }
        size_t count = std::min(num_samples, m_ring.available());
        const uint64_t later = m_discontinuity.load(std::memory_order_acquire);
        const uint64_t later_generation = later >> k_position_bits;
        if (later_generation != m_seen_generation) {
            // Marks came while counting. One, ahead of the reader: read up to it, report it next
            // time. Otherwise act on the newest first.
            const uint64_t position = later & k_position_mask;
            const uint64_t read_position = m_read & k_position_mask;
            if (later_generation != m_seen_generation + 1 || position <= read_position) {
                continue;
            }
            count = std::min<size_t>(count, position - read_position);
        }
        result.m_read = m_ring.read(channels, count);
        m_read += result.m_read;
        return result;
    }
    return result;  // the writer kept marking: nothing safe to read this time
}

}  // namespace thl::inline THL_ABI_NAMESPACE::dsp::probe
