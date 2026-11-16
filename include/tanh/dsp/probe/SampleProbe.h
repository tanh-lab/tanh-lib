#pragma once

#include <tanh/core/AbiNamespace.h>
#include <tanh/core/BufferView.h>
#include <tanh/core/Exports.h>
#include <tanh/core/threading/SpscRingBuffer.h>
#include <tanh/dsp/probe/Probe.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace thl::inline THL_ABI_NAMESPACE::dsp::probe {

/**
 * @brief Hands the samples it watches to one reader thread: the source of a scope, a spectrum
 * analyser, a goniometer.
 *
 * observe() copies every block into an SpscRingBuffer (one copy per channel, nothing else on the
 * audio thread). One reader thread drains it with read(), keeps what it needs in a history of its
 * own and does the analysis there.
 *
 * Discontinuities: wherever the stream is not continuous, the writer marks the position: after a
 * reset() (also the one after the probe was inactive) and where samples were dropped because the
 * reader fell behind. read() skips everything before the newest mark it has not seen and reports
 * it (ReadResult::m_discontinuity): the reader then starts its history over instead of joining
 * samples across the gap.
 *
 * The ring holds capacity_seconds() at the prepared sample rate (and at least two blocks): a
 * reader at screen rate (16 ms) has plenty of room.
 *
 * Threads: as ProbeBase (no reader during prepare()); read(), available() and the read
 * position belong to the one reader; dropped() and sample_rate() to any thread.
 */
class TANH_API SampleProbe : public ProbeBase {
public:
    static constexpr double k_default_capacity_seconds = 0.25;

    /// What read() did.
    struct ReadResult {
        size_t m_read = 0;             ///< samples copied
        bool m_discontinuity = false;  ///< they do not continue the previous read: start over
    };

    explicit SampleProbe(double capacity_seconds = k_default_capacity_seconds);

    void prepare(double sample_rate, size_t max_block_size, size_t num_channels) override;
    /// Audio thread: marks a discontinuity; the reader skips what came before it.
    void reset() TANH_NONBLOCKING_FUNCTION override;

    [[nodiscard]] double capacity_seconds() const { return m_capacity_seconds; }
    [[nodiscard]] double sample_rate() const {
        return m_sample_rate.load(std::memory_order_relaxed);
    }
    [[nodiscard]] size_t num_channels() const { return m_ring.num_channels(); }
    [[nodiscard]] size_t capacity() const { return m_ring.capacity(); }

    /// Reader: copies up to @p num_samples of what has arrived, oldest first, into @p channels
    /// (@p channels[c]: channel c). After a discontinuity it first skips what came before it.
    ReadResult read(std::span<float* const> channels, size_t num_samples) TANH_NONBLOCKING_FUNCTION;
    /// Reader: the samples that have arrived and not been read (before a discontinuity included).
    [[nodiscard]] size_t available() const { return m_ring.available(); }
    /// Any thread: the samples dropped because the reader fell behind, since prepare().
    [[nodiscard]] size_t dropped() const { return m_ring.dropped(); }

protected:
    void observe(const thl::core::BufferView& block) TANH_NONBLOCKING_FUNCTION override;

private:
    /// Writer: the stream is not continuous at the current position.
    void mark_discontinuity() TANH_NONBLOCKING_FUNCTION;

    static constexpr int k_position_bits = 48;
    static constexpr int k_max_read_attempts = 4;
    static constexpr uint64_t k_position_mask = (uint64_t{1} << k_position_bits) - 1;

    double m_capacity_seconds;
    std::atomic<double> m_sample_rate{48000.0};
    std::vector<const float*> m_channel_pointers;  // one block's, for write()
    thl::core::SpscRingBuffer<float> m_ring;
    uint64_t m_written = 0;  // writer: samples written into the ring
    // The newest discontinuity: its generation (high bits) and stream position (low bits), one
    // atomic so the reader never sees one without the other.
    std::atomic<uint64_t> m_discontinuity{0};
    uint64_t m_read = 0;             // reader: samples consumed from the ring
    uint64_t m_seen_generation = 0;  // reader: the discontinuity read() acted on last
};

}  // namespace thl::inline THL_ABI_NAMESPACE::dsp::probe
