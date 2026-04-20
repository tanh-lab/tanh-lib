#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include <readerwriterqueue.h>

#include <tanh/core/Exports.h>

namespace thl::modulation {

// Pure-transport helper for input-driven modulation sources (XY pad, touch
// LFO, MIDI-CC, haptic). Bridges a UI-thread producer to an audio-thread
// consumer via a lock-free SPSC ring buffer, then spreads drained events
// within the current audio block at offsets i * block_size / n_bucket per
// stream.
//
// Events carry no timestamp. Within each stream (mono + each voice), the
// i-th of n_bucket events is placed at a monotonically increasing offset —
// FIFO ordering is preserved per stream, cross-stream ordering is discarded.
// Per-bucket (not global) spreading ensures polyphonic simultaneity: 10
// concurrent touch events on 10 voices all land at offset 0, not strummed.
//
// Not a base class — compose this as a member of a ModulationSource and
// call drain_spread() from pre_process_block(). Mirrors ModulationSource's
// has_mono / num_voices capability flags.
class TANH_API InputEventQueue {
public:
    enum class EventType : uint8_t { Value, Active };

    struct Event {
        EventType m_type;
        bool m_is_mono;   // true → mono stream; false → per-voice stream
        uint8_t m_voice;  // unused when m_is_mono = true
        bool m_active;    // used when m_type == Active
        float m_value;    // used when m_type == Value
    };

    // Configure which input streams this queue carries.
    //   has_mono = true      enables push_mono_* + a mono drain bucket.
    //   num_voices > 0       enables push_voice_* + per-voice drain buckets.
    //   queue_capacity       SPSC queue size; also reserves drain scratch.
    explicit InputEventQueue(bool has_mono = true,
                             uint32_t num_voices = 0,
                             size_t queue_capacity = 64);

    // Reserve all audio-thread scratch buffers. Call from the composing
    // ModulationSource's prepare() before any audio-thread use.
    void prepare();

    // ── UI thread (single producer) — non-blocking, allocation-free ────
    // Returns false if the SPSC queue is full (audio-thread stall — always
    // an error). Mono/voice push methods assert on misuse.
    bool push_mono_value(float value);
    bool push_mono_active(bool active);
    bool push_voice_value(uint32_t voice, float value);
    bool push_voice_active(uint32_t voice, bool active);

    // ── Audio thread — drain the queue ─────────────────────────────────
    // Call exactly once per block from pre_process_block(). Snapshots the
    // queue, buckets events per stream (mono + each voice), spreads within
    // each bucket at offsets i * block_size / n_bucket, invokes cb per
    // event. Returns total events dispatched.
    using OnEvent = std::function<void(const Event& e, uint32_t offset)>;
    size_t drain_spread(uint32_t block_size, const OnEvent& cb);

    [[nodiscard]] bool has_mono() const { return m_has_mono; }
    [[nodiscard]] uint32_t num_voices() const { return m_num_voices; }
    [[nodiscard]] size_t queue_capacity() const { return m_queue_capacity; }

private:
    // Layout ordered for minimal padding: largest alignment first, smallest
    // scalars last (reported by clang-analyzer-optin.performance.Padding).
    // Cross-thread transport — lock-free SPSC.
    moodycamel::ReaderWriterQueue<Event> m_event_queue;

    // Audio-thread-only scratch (no sync needed). Distinct storage per
    // stream mirrors ModulationSource's own mono/voice buffer split.
    std::vector<Event> m_drain_buffer;
    std::vector<size_t> m_mono_indices;                // only if m_has_mono
    std::vector<std::vector<size_t>> m_voice_indices;  // size = m_num_voices

    const size_t m_queue_capacity;
    const uint32_t m_num_voices;
    const bool m_has_mono;
};

}  // namespace thl::modulation
