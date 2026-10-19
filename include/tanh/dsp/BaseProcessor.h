#pragma once

#include <tanh/core/BufferView.h>
#include <tanh/core/Exports.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <span>

namespace thl::dsp {

class TANH_API BaseProcessor {
public:
    BaseProcessor() = default;
    virtual ~BaseProcessor() = default;

    BaseProcessor(const BaseProcessor& other) noexcept
        : m_latency_samples(other.latency_samples())
        , m_max_latency_samples(other.max_latency_samples()) {}

    BaseProcessor& operator=(const BaseProcessor& other) noexcept {
        m_latency_samples.store(other.latency_samples(), std::memory_order_relaxed);
        m_max_latency_samples.store(other.max_latency_samples(), std::memory_order_relaxed);
        return *this;
    }

    virtual void prepare(const double& sample_rate,
                         const size_t& samples_per_block,
                         const size_t& num_channels) = 0;
    virtual void process(thl::core::BufferView buffer,
                         uint32_t modulation_offset = 0) TANH_NONBLOCKING_FUNCTION = 0;

    // Splits @p buffer at the change points and calls process() per part with the
    // part's position in the block. @p buffer starts at sample @p modulation_offset
    // of the block (a caller that splits the block itself, e.g. a Graph run in
    // parts); change points are positions in the block.

    // Self-driven: calls get_change_points() to obtain split positions
    void process_modulated(const thl::core::BufferView& buffer,
                           uint32_t modulation_offset = 0) TANH_NONBLOCKING_FUNCTION;

    // Caller-injected: uses externally supplied change points
    void process_modulated(const thl::core::BufferView& buffer,
                           std::span<const uint32_t> change_points,
                           uint32_t modulation_offset = 0) TANH_NONBLOCKING_FUNCTION;

    uint32_t latency_samples() const TANH_NONBLOCKING_FUNCTION {
        return m_latency_samples.load(std::memory_order_relaxed);
    }
    uint32_t max_latency_samples() const TANH_NONBLOCKING_FUNCTION {
        return m_max_latency_samples.load(std::memory_order_relaxed);
    }

protected:
    virtual std::span<const uint32_t> get_change_points() TANH_NONBLOCKING_FUNCTION { return {}; }

    void set_latency_samples(uint32_t samples) TANH_NONBLOCKING_FUNCTION {
        const uint32_t max = max_latency_samples();
        assert(samples <= max);
        m_latency_samples.store(std::min(samples, max), std::memory_order_relaxed);
    }

    void set_max_latency_samples(uint32_t max) {
        m_max_latency_samples.store(max, std::memory_order_relaxed);
    }

private:
    void split_and_process(const thl::core::BufferView& buffer,
                           std::span<const uint32_t> change_points,
                           uint32_t modulation_offset) TANH_NONBLOCKING_FUNCTION;

    std::atomic<uint32_t> m_latency_samples{0};
    std::atomic<uint32_t> m_max_latency_samples{0};
};

}  // namespace thl::dsp
