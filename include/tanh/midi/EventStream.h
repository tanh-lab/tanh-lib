#pragma once

#include <tanh/core/AbiNamespace.h>
#include <tanh/core/Exports.h>
#include <tanh/midi/AudioMidiProcessor.h>
#include <tanh/midi/Event.h>
#include <tanh/midi/EventDecoder.h>
#include <tanh/midi/VoiceAllocator.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace thl::inline THL_ABI_NAMESPACE::midi {

/// Sizes of an EventStream.
struct EventStreamConfig {
    /// Events one block can hold. A note on is four events (the note and its three
    /// expressions), a bend on an MPE manager channel one per sounding voice.
    std::size_t m_max_events = 512;
    /// Voices notes are spread over (VoiceAllocator::set_voice_count()).
    std::uint32_t m_voice_count = 16;
};

/// The MIDI of one audio block as events with voices: the list everything downstream
/// reads — modulation sources (thl::modulation::MidiControllerSource, MidiNoteSource), a
/// Monitor, the engine itself.
///
/// Once per block, on the audio thread and before anything reads it, process() takes the
/// block's length and its packets (as AudioMidiProcessor::process() gets them, or as a plugin
/// wrapper builds them from its host's buffer), runs them through an EventDecoder and a
/// VoiceAllocator and keeps the result until the next call. The events are in ascending
/// sample offset.
///
/// Threading: prepare() with the audio stopped (it allocates); process(), events() and
/// dropped() on the audio thread; release_all() and the setters of voices() on any thread.
class TANH_API EventStream {
public:
    EventStream() = default;

    /// Allocates for `config`; forgets every note and controller state.
    void prepare(const EventStreamConfig& config = EventStreamConfig{});

    /// Replaces the list with the events of `block`, a block of `num_samples` frames;
    /// BlockEvents that are not MIDI are skipped. Call it every block, with an empty span
    /// when nothing arrived.
    void process(std::span<const BlockEvent> block,
                 std::size_t num_samples) noexcept TANH_NONBLOCKING_FUNCTION;

    /// Length of the block last processed, in frames: what the readers of events() render.
    [[nodiscard]] std::size_t num_samples() const noexcept TANH_NONBLOCKING_FUNCTION {
        return m_num_samples;
    }

    /// The events of the block last processed.
    [[nodiscard]] std::span<const Event> events() const noexcept TANH_NONBLOCKING_FUNCTION {
        return {m_events.data(), m_size};
    }

    /// Events that did not fit a block's list, since prepare() (wraps at 2^32). Notes may
    /// hang after a drop; release_all() ends them.
    [[nodiscard]] std::uint32_t dropped() const noexcept {
        return m_dropped.load(std::memory_order_relaxed);
    }

    /// Ends every sounding note at the start of the next block: a panic button, and the
    /// answer to lost input (InputQueue::dropped() rose).
    void release_all() noexcept { m_release_all.store(true, std::memory_order_release); }

    /// The voices: MPE zones, bend ranges, and what each voice plays.
    [[nodiscard]] VoiceAllocator& voices() noexcept { return m_voices; }
    [[nodiscard]] const VoiceAllocator& voices() const noexcept { return m_voices; }

private:
    EventDecoder m_decoder;
    VoiceAllocator m_voices;
    std::vector<Event> m_events;
    std::size_t m_size = 0;
    std::size_t m_num_samples = 0;
    std::atomic<std::uint32_t> m_dropped{0};
    std::atomic<bool> m_release_all{false};
};

}  // namespace thl::inline THL_ABI_NAMESPACE::midi
