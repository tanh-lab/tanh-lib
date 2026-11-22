#include <tanh/core/AbiNamespace.h>
#include <tanh/midi/AudioMidiProcessor.h>
#include <tanh/midi/BlockEventCollector.h>
#include <tanh/midi/InputQueue.h>
#include <tanh/midi/StreamClock.h>
#include <tanh/midi/Ump.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace thl::inline THL_ABI_NAMESPACE::midi {

BlockEventCollector::BlockEventCollector(const BlockEventCollectorConfig& config)
    : m_config(config) {}

void BlockEventCollector::prepare(double sample_rate, std::uint32_t max_block_frames) {
    m_clock.prepare(sample_rate, max_block_frames, m_config.m_latency_ns);
    m_held.assign(m_config.m_max_held, TimedUmp{});
    m_events.assign(m_config.m_max_events_per_block, BlockEvent{});
    m_held_count = 0;
    m_event_count = 0;
}

void BlockEventCollector::reset() noexcept {
    m_clock.reset();
    m_held_count = 0;
    m_event_count = 0;
}

std::span<const BlockEvent> BlockEventCollector::collect(InputQueue& queue,
                                                         std::int64_t now_ns,
                                                         std::uint32_t frames) noexcept {
    const StreamClock::Block block = m_clock.begin_block(now_ns, frames);

    // Everything that has arrived joins the waiting packets, behind the ones already there.
    const std::size_t limit = queue.capacity();
    TimedUmp packet;
    for (std::size_t taken = 0; taken < limit && queue.pop(packet); ++taken) {
        if (m_held_count < m_held.size()) {
            m_held[m_held_count++] = packet;
        } else {
            m_dropped.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Packets that fall into this block become events; the rest stay, in order.
    m_event_count = 0;
    std::size_t kept = 0;
    for (std::size_t i = 0; i < m_held_count; ++i) {
        std::uint32_t offset = 0;
        const bool due = block.offset_of(m_held[i].m_time_ns, offset);
        if (due && m_event_count < m_events.size()) {
            BlockEvent& event = m_events[m_event_count++];
            event.m_type = BlockEvent::Type::Midi;
            event.m_midi = m_held[i].m_ump;
            event.m_sample_offset = offset;
            event.m_source = m_held[i].m_source;
        } else {
            m_held[kept++] = m_held[i];
        }
    }
    m_held_count = kept;

    // Ascending offset, equal offsets in arrival order. The list is nearly sorted already
    // (one input delivers in time order), so an insertion sort is the cheap one.
    for (std::size_t i = 1; i < m_event_count; ++i) {
        const BlockEvent event = m_events[i];
        std::size_t pos = i;
        while (pos > 0 && m_events[pos - 1].m_sample_offset > event.m_sample_offset) {
            m_events[pos] = m_events[pos - 1];
            --pos;
        }
        m_events[pos] = event;
    }

    return {m_events.data(), m_event_count};
}

}  // namespace thl::inline THL_ABI_NAMESPACE::midi
