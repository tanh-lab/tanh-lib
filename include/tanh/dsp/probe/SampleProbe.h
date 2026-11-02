#pragma once

#include <tanh/core/AbiNamespace.h>
#include <tanh/core/BufferView.h>
#include <tanh/core/Exports.h>
#include <tanh/core/threading/SpscRingBuffer.h>
#include <tanh/dsp/probe/Probe.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <cstddef>
#include <span>
#include <vector>

namespace thl::inline THL_ABI_NAMESPACE::dsp::probe {

/**
 * @brief Hands the samples it watches to one reader thread: the source of a scope, a spectrum
 * analyser, a goniometer.
 *
 * observe() copies every block into an SpscRingBuffer (one copy per channel, nothing else on the
 * audio thread). One reader thread drains it with read(), keeps what it needs in a history of its
 * own and does the analysis there. A reader that falls behind loses the newest samples, counted
 * in dropped(): it should then start its history over rather than join samples across the gap.
 *
 * The ring holds capacity_seconds() at the prepared sample rate (and at least two blocks): a
 * reader at screen rate (16 ms) has plenty of room.
 *
 * Threads: as ProbeBase; read(), available() and skip() belong to the one reader, dropped() and
 * sample_rate() to any thread.
 */
class TANH_API SampleProbe : public ProbeBase {
public:
    static constexpr double k_default_capacity_seconds = 0.25;

    explicit SampleProbe(double capacity_seconds = k_default_capacity_seconds);

    void prepare(double sample_rate, size_t max_block_size, size_t num_channels) override;

    [[nodiscard]] double capacity_seconds() const { return m_capacity_seconds; }
    [[nodiscard]] double sample_rate() const { return m_sample_rate; }
    [[nodiscard]] size_t num_channels() const { return m_ring.num_channels(); }
    [[nodiscard]] size_t capacity() const { return m_ring.capacity(); }

    /// Reader: copies up to @p num_samples of what has arrived, oldest first, into @p channels
    /// (SpscRingBuffer::read()). Returns the samples read.
    size_t read(std::span<float* const> channels, size_t num_samples) {
        return m_ring.read(channels, num_samples);
    }
    /// Reader: the samples that have arrived and not been read.
    [[nodiscard]] size_t available() const { return m_ring.available(); }
    /// Reader: drops up to @p num_samples of the oldest arrived samples.
    size_t skip(size_t num_samples) { return m_ring.skip(num_samples); }
    /// Any thread: the samples dropped because the reader fell behind, since prepare().
    [[nodiscard]] size_t dropped() const { return m_ring.dropped(); }

protected:
    void observe(const thl::core::BufferView& block) TANH_NONBLOCKING_FUNCTION override;

private:
    double m_capacity_seconds;
    double m_sample_rate = 48000.0;
    std::vector<const float*> m_channel_pointers;  // one block's, for write()
    thl::core::SpscRingBuffer<float> m_ring;
};

}  // namespace thl::inline THL_ABI_NAMESPACE::dsp::probe
