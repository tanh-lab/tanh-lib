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

/// Where in a block the collector puts a packet.
enum class EventPlacement : std::uint8_t {
    /// At the sample offset of its timestamp. Every event is late by the same amount, one
    /// full-size block (plus m_latency_ns), and events keep their distances to the sample.
    /// What a plugin host does, and the default.
    Exact,
    /// At the start of the first block asked for after it arrived: the least delay there
    /// is, between nothing and one block, at the price of timing that is quantised to the
    /// block. For very small blocks, where that is too fine to hear.
    Immediate,
};

/// Settings of a BlockEventCollector.
struct BlockEventCollectorConfig {
    EventPlacement m_placement = EventPlacement::Exact;
    /// The most events one block gets. More than that wait for the next block.
    std::size_t m_max_events_per_block = 256;
    /// The most packets that can wait: for the next block, or for their time to come.
    std::size_t m_max_held = 1024;
    /// EventPlacement::Exact only: added to the one block every event is late by
    /// (StreamClock). Slack for a MIDI thread that hands a message over later than its
    /// timestamp; such a message otherwise lands at the start of the next block instead of
    /// its exact place. 0 by default: the least delay exact placement allows.
    std::int64_t m_latency_ns = 0;
};

/// Turns the MIDI input queue into the event list of each audio block, every packet at the
/// sample offset its arrival time maps to (StreamClock).
///
/// Per block: take what has arrived, give every packet whose time falls into the block's
/// window its offset, and keep the ones that are not due yet for a later block. A packet
/// older than the window goes to offset 0. The list is in ascending offset; packets with
/// the same offset keep the order they arrived in. With EventPlacement::Immediate every
/// packet whose time has come goes to offset 0 instead.
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

    /// How late an event is rendered against its arrival, in nanoseconds. Exact placement:
    /// the largest block plus the configured latency, for every event. Immediate: 0, the
    /// least it can be — an event then waits for the next block, up to one block.
    [[nodiscard]] std::int64_t latency_ns() const noexcept {
        return m_config.m_placement == EventPlacement::Exact ? m_clock.latency_ns() : 0;
    }

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
