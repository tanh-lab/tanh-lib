#include "tanh/modulation/InputEventQueue.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <tanh/state/ModulationScope.h>

namespace thl::modulation {

InputEventQueue::InputEventQueue(thl::modulation::ModulationScope scope, size_t queue_capacity)
    : m_event_queue(queue_capacity), m_queue_capacity(queue_capacity), m_scope(scope) {}

void InputEventQueue::prepare(uint32_t num_voices) {
    // Global scope carries the mono stream only — per-voice buckets are not
    // allocated regardless of the num_voices argument (the matrix passes
    // voice_count(global) == 1, which would otherwise spuriously allocate a
    // voice bucket that push_voice_* can never reach). Non-global: size the
    // voice-bucket array to match the scope's voice count.
    if (has_mono()) {
        m_num_voices = 0;
        m_voice_indices.clear();
    } else {
        m_num_voices = num_voices;
        m_voice_indices.resize(m_num_voices);
    }

    // Pre-reserve every audio-thread-local vector so drain_spread never
    // allocates on the hot path. Worst case: all drained events target the
    // same stream, so each bucket needs queue_capacity capacity.
    m_drain_buffer.clear();
    m_drain_buffer.reserve(m_queue_capacity);

    if (has_mono()) {
        m_mono_indices.clear();
        m_mono_indices.reserve(m_queue_capacity);
    }

    for (auto& bucket : m_voice_indices) {
        bucket.clear();
        bucket.reserve(m_queue_capacity);
    }
}

bool InputEventQueue::push_mono_value(float value) {
    assert(has_mono() && "push_mono_value on a voice-only queue");
    Event const e{.m_type = EventType::Value,
                  .m_is_mono = true,
                  .m_voice = 0,
                  .m_active = false,
                  .m_value = value};
    return m_event_queue.try_enqueue(e);
}

bool InputEventQueue::push_mono_active(bool active) {
    assert(has_mono() && "push_mono_active on a voice-only queue");
    Event const e{.m_type = EventType::Active,
                  .m_is_mono = true,
                  .m_voice = 0,
                  .m_active = active,
                  .m_value = 0.0f};
    return m_event_queue.try_enqueue(e);
}

bool InputEventQueue::push_voice_value(uint32_t voice, float value) {
    assert(m_num_voices > 0 && "push_voice_value on a mono-only queue");
    assert(voice < m_num_voices && "voice index out of range");
    Event const e{.m_type = EventType::Value,
                  .m_is_mono = false,
                  .m_voice = static_cast<uint8_t>(voice),
                  .m_active = false,
                  .m_value = value};
    return m_event_queue.try_enqueue(e);
}

bool InputEventQueue::push_voice_active(uint32_t voice, bool active) {
    assert(m_num_voices > 0 && "push_voice_active on a mono-only queue");
    assert(voice < m_num_voices && "voice index out of range");
    Event const e{.m_type = EventType::Active,
                  .m_is_mono = false,
                  .m_voice = static_cast<uint8_t>(voice),
                  .m_active = active,
                  .m_value = 0.0f};
    return m_event_queue.try_enqueue(e);
}

size_t InputEventQueue::drain_spread(uint32_t block_size, const OnEvent& cb) {
    // 1. Snapshot the SPSC queue into pre-reserved scratch (no allocation).
    m_drain_buffer.clear();
    Event evt{};  // NOLINT(misc-const-correctness) — mutated by try_dequeue
    while (m_event_queue.try_dequeue(evt)) { m_drain_buffer.push_back(evt); }
    if (m_drain_buffer.empty()) { return 0; }

    // 2. Bucket events by stream (mono or per-voice). Stable within bucket.
    if (has_mono()) { m_mono_indices.clear(); }
    for (auto& bucket : m_voice_indices) { bucket.clear(); }

    for (size_t i = 0; i < m_drain_buffer.size(); ++i) {
        const Event& e = m_drain_buffer[i];
        if (e.m_is_mono) {
            if (has_mono()) { m_mono_indices.push_back(i); }
        } else {
            if (e.m_voice < m_num_voices) { m_voice_indices[e.m_voice].push_back(i); }
        }
    }

    // 3. Spread within each bucket independently. FIFO preserved per stream.
    auto dispatch_bucket = [&](const std::vector<size_t>& idx) {
        const size_t n = idx.size();
        if (n == 0) { return; }
        for (size_t i = 0; i < n; ++i) {
            const auto offset = static_cast<uint32_t>((i * static_cast<size_t>(block_size)) / n);
            cb(m_drain_buffer[idx[i]], offset);
        }
    };

    if (has_mono()) { dispatch_bucket(m_mono_indices); }
    for (uint32_t v = 0; v < m_num_voices; ++v) { dispatch_bucket(m_voice_indices[v]); }

    return m_drain_buffer.size();
}

}  // namespace thl::modulation
