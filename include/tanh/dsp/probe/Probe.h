#pragma once

#include <tanh/core/AbiNamespace.h>
#include <tanh/core/BufferView.h>
#include <tanh/core/Exports.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace thl::inline THL_ABI_NAMESPACE::dsp::probe {

/**
 * @brief Something that watches a signal without changing it: a meter, an analyser's tap.
 *
 * Not a BaseProcessor: a probe gets every block read-only, is never a node of a graph (attaching
 * or removing one is not an edit of its structure) and has no latency, dry signal or parameters
 * of its own. Whoever runs the signal (a Graph, a processor) calls process() with every block at
 * the point the probe watches; the probe does the little the audio thread must do (copy the
 * block, keep a peak) and leaves the rest (FFT, smoothing, drawing) to its reader.
 *
 * Threads: prepare() may allocate and runs on the control thread with audio stopped (or before
 * the probe is attached); readers must not read during it either (hosts call it on the main
 * thread, so readers on the main thread, e.g. a UI timer, are safe). process(), observe() and
 * reset() run on the audio thread. set_active(), is_active() and latency_samples() may be called
 * from any thread.
 */
class TANH_API ProbeBase {
public:
    ProbeBase() = default;
    virtual ~ProbeBase();
    ProbeBase(const ProbeBase&) = delete;
    ProbeBase& operator=(const ProbeBase&) = delete;

    /// Control thread: blocks of up to @p max_block_size samples and @p num_channels channels will
    /// come at @p sample_rate.
    virtual void prepare(double sample_rate, size_t max_block_size, size_t num_channels) = 0;

    /// Audio thread: one block at the watched point, @p latency_samples behind the signal's input
    /// there (so a reader can align two probes, e.g. a node's input and output). Skipped while
    /// inactive; the first block after the probe was inactive starts with reset(), so nothing
    /// from before the pause is joined to what follows it.
    void process(const thl::core::BufferView& block,
                 uint32_t latency_samples) TANH_NONBLOCKING_FUNCTION {
        if (!m_active.load(std::memory_order_relaxed)) {
            m_was_inactive = true;
            return;
        }
        if (m_was_inactive) {
            m_was_inactive = false;
            reset();
        }
        m_latency_samples.store(latency_samples, std::memory_order_relaxed);
        observe(block);
    }

    /// Audio thread: forgets what it has seen, as after prepare(): the next block starts anew.
    virtual void reset() TANH_NONBLOCKING_FUNCTION {}

    /// Any thread: an inactive probe costs nothing, e.g. an analyser that is not visible.
    void set_active(bool active) { m_active.store(active, std::memory_order_relaxed); }
    [[nodiscard]] bool is_active() const { return m_active.load(std::memory_order_relaxed); }

    /// Any thread: the latency of the last block observed. Not tied to the samples a reader has
    /// drained (it may already be the next block's): enough to align displays, not single samples.
    [[nodiscard]] uint32_t latency_samples() const {
        return m_latency_samples.load(std::memory_order_relaxed);
    }

protected:
    /// Audio thread: what the probe does with a block. Must not change it, allocate or wait.
    virtual void observe(const thl::core::BufferView& block) TANH_NONBLOCKING_FUNCTION = 0;

private:
    std::atomic<bool> m_active{true};
    std::atomic<uint32_t> m_latency_samples{0};
    bool m_was_inactive = false;  // audio thread only
};

}  // namespace thl::inline THL_ABI_NAMESPACE::dsp::probe
