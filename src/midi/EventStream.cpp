#include <tanh/core/AbiNamespace.h>
#include <tanh/midi/AudioMidiProcessor.h>
#include <tanh/midi/Event.h>
#include <tanh/midi/EventStream.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace thl::inline THL_ABI_NAMESPACE::midi {

namespace {

// EventDecoder makes at most two events of one packet.
constexpr std::size_t k_max_events_per_packet = 2;

}  // namespace

void EventStream::prepare(const EventStreamConfig& config) {
    m_events.assign(std::max<std::size_t>(config.m_max_events, 1), Event{});
    m_size = 0;
    m_num_samples = 0;
    m_decoder.reset();
    m_voices.set_voice_count(config.m_voice_count);
    m_dropped.store(0, std::memory_order_relaxed);
    m_release_all.store(false, std::memory_order_relaxed);
}

void EventStream::process(std::span<const BlockEvent> block,
                          std::size_t num_samples) noexcept TANH_NONBLOCKING_FUNCTION {
    EventWriter out{std::span<Event>{m_events}};
    m_num_samples = num_samples;

    if (m_release_all.exchange(false, std::memory_order_acquire)) { m_voices.release_all(0, out); }
    m_voices.begin_block(out);

    for (const BlockEvent& packet : block) {
        if (packet.m_type != BlockEvent::Type::Midi) { continue; }
        std::array<Event, k_max_events_per_packet> scratch{};
        EventWriter decoded{std::span<Event>{scratch}};
        m_decoder.decode(packet.m_midi, packet.m_sample_offset, packet.m_source, decoded);
        for (const Event& event : decoded.events()) { m_voices.process(event, out); }
    }

    m_size = out.size();
    if (out.dropped() != 0) {
        m_dropped.fetch_add(static_cast<std::uint32_t>(out.dropped()), std::memory_order_relaxed);
    }
}

}  // namespace thl::inline THL_ABI_NAMESPACE::midi
