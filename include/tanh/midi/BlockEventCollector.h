#pragma once

#include <tanh/core/AbiNamespace.h>
#include <tanh/core/Exports.h>
#include <tanh/midi/AudioMidiProcessor.h>
#include <tanh/midi/InputQueue.h>
#include <tanh/midi/StreamClock.h>
#include <tanh/midi/Ump.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace thl::inline THL_ABI_NAMESPACE::midi {

/// Settings of a BlockEventCollector.
struct BlockEventCollectorConfig {
    /// The most events one block gets. More than that wait for the next block.
    std::size_t m_max_events_per_block = 256;
    /// The most packets that can wait: for the next block, or for their time to come.
    std::size_t m_max_held = 1024;
    /// Added to the one (largest) block every event is late by (StreamClock): slack for the MIDI
    /// thread, so that a message handed over a little after its timestamp still gets its
    /// exact place instead of the start of the next block.
    std::int64_t m_latency_ns = 1'000'000;
};

/// Turns the MIDI input queue into the event list of each audio block, every packet at the
/// sample offset its arrival time maps to (StreamClock).
///
/// Per block: take what has arrived, give every packet whose time falls into the block's
/// window its offset, and keep the ones that are not due yet for a later block. A packet
/// older than the window goes to offset 0. The list is in ascending offset; packets with
/// the same offset keep the order they arrived in.
///
/// prepare() allocates; collect() and reset() are for the audio thread and neither
/// allocate nor block.
class TANH_API BlockEventCollector {
public:
    explicit BlockEventCollector(const BlockEventCollectorConfig& config = {});

    /// Sets the sample rate and the largest block the stream will ask for, and allocates.
    /// Control thread.
    void prepare(double sample_rate, std::uint32_t max_block_frames);

    /// Drops what is waiting and starts a new stream.
    void reset() noexcept TANH_NONBLOCKING_FUNCTION;

    /// The events of the block of `frames` frames that is asked for at `now_ns` (the clock
    /// of core::uptime_ns()). Call once per block, first thing in the audio callback. The
    /// span stays valid until the next call.
    [[nodiscard]] std::span<const BlockEvent> collect(InputQueue& queue,
                                                      std::int64_t now_ns,
                                                      std::uint32_t frames) noexcept
        TANH_NONBLOCKING_FUNCTION;

    /// Packets lost because more than m_max_held were waiting, since construction.
    [[nodiscard]] std::uint32_t dropped() const noexcept TANH_NONBLOCKING_FUNCTION {
        return m_dropped.load(std::memory_order_relaxed);
    }

    /// How late every event is rendered against its arrival, in nanoseconds: the largest
    /// block plus the configured latency.
    [[nodiscard]] std::int64_t latency_ns() const noexcept { return m_clock.latency_ns(); }

private:
    BlockEventCollectorConfig m_config;
    StreamClock m_clock;
    std::vector<TimedUmp> m_held;      // waiting packets, in arrival order; m_held_count valid
    std::vector<BlockEvent> m_events;  // this block's events; m_event_count valid
    std::size_t m_held_count = 0;
    std::size_t m_event_count = 0;
    std::atomic<std::uint32_t> m_dropped{0};
};

}  // namespace thl::inline THL_ABI_NAMESPACE::midi
