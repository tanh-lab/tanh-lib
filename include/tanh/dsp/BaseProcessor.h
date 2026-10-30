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

    /// process(), also writing the dry signal into @p dry: the input with the phase response
    /// of process() but without its effect, so wet and dry can be blended. Undelayed: a processor
    /// with latency_samples() leaves aligning the dry signal to its latency to whoever mixes it
    /// (a Graph delays a node's dry path by it). The default copies @p buffer into @p dry;
    /// override it if process() changes the phase. An empty @p dry means no dry signal.
    virtual void process_with_dry(thl::core::BufferView buffer,
                                  uint32_t modulation_offset,
                                  thl::core::BufferView dry) TANH_NONBLOCKING_FUNCTION;

    /// Clears what the processor holds from the audio it has processed (delay lines, filter
    /// states, envelopes), keeping its settings, as if no audio had passed. Real-time safe: a
    /// Graph calls it on the audio thread when a bypassed node comes back, so the node does not
    /// resume where it froze. The default does nothing.
    virtual void reset() TANH_NONBLOCKING_FUNCTION {}

    // Splits @p buffer (and @p dry with it) at the change points and calls process() (or
    // process_with_dry()) per part
    // with the part's position in the block. @p buffer starts at sample @p modulation_offset of
    // the block (a caller that splits the block itself, e.g. a Graph run in parts); change points
    // are positions in the block.

    // Self-driven: calls get_change_points() to obtain split positions
    void process_modulated(const thl::core::BufferView& buffer,
                           uint32_t modulation_offset = 0) TANH_NONBLOCKING_FUNCTION;
    void process_modulated_with_dry(const thl::core::BufferView& buffer,
                                    const thl::core::BufferView& dry,
                                    uint32_t modulation_offset = 0) TANH_NONBLOCKING_FUNCTION;

    // Caller-injected: uses externally supplied change points
    void process_modulated(const thl::core::BufferView& buffer,
                           std::span<const uint32_t> change_points,
                           uint32_t modulation_offset = 0) TANH_NONBLOCKING_FUNCTION;
    void process_modulated_with_dry(const thl::core::BufferView& buffer,
                                    std::span<const uint32_t> change_points,
                                    const thl::core::BufferView& dry,
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
                           uint32_t modulation_offset,
                           const thl::core::BufferView& dry = {}) TANH_NONBLOCKING_FUNCTION;

    std::atomic<uint32_t> m_latency_samples{0};
    std::atomic<uint32_t> m_max_latency_samples{0};
};

}  // namespace thl::dsp
