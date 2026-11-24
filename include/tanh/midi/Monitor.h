#pragma once

#include <tanh/core/AbiNamespace.h>
#include <tanh/core/Exports.h>
#include <tanh/core/threading/LockFreeQueue.h>
#include <tanh/midi/Event.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace thl::inline THL_ABI_NAMESPACE::midi {

/// What the MIDI input is doing, for a user interface: the audio thread shows it every
/// block's events, any other thread reads the state they left behind.
///
/// Three views, all without locks:
///
/// - State: the last value of every controller, pitch bend and pressure per channel, and
///   which keys are held with which velocity — for meters, a keyboard, a controller list.
/// - last_controller(): the controller that moved last, with a serial that changes on
///   every move — what MIDI learn waits for.
/// - drain(): the events themselves in order, for a scrolling log. Bounded: when nobody
///   drains, new events are left out of the log (log_dropped()) and the state stays right.
///
/// - voice(): what each voice plays, with its tuning, pressure and timbre — the view for
///   MPE and MIDI 2.0 per-note expression. In an MPE zone the bend, pressure and CC 74 of
///   a member channel belong to a note, so they show up here and not under pitch_bend(),
///   channel_pressure(), controller() or last_controller().
///
/// State is kept per channel; the 16 groups and the inputs share it.
///
/// Threading: observe() and reset() on the audio thread (no allocation, no locks); the
/// getters on any thread; drain() on one thread at a time.
class TANH_API Monitor {
public:
    /// The controller that moved last.
    struct LastController {
        /// Rises with every controller event; 0 until the first one.
        std::uint32_t m_serial = 0;
        /// Controller, AssignableController, PitchBend or ChannelPressure.
        EventType m_type = EventType::Controller;
        std::uint8_t m_channel = 0;
        std::uint16_t m_index = 0;  ///< Controller or NRPN number; 0 for the other two.
        float m_value = 0.0f;       ///< Its value then, or that of a move right after it.
    };

    /// What a voice plays, as far as the events said (VoiceAllocator's voices).
    struct VoiceView {
        bool m_active = false;       ///< Between its note on and note off.
        std::uint8_t m_channel = 0;  ///< Of the note it plays or played last.
        std::uint8_t m_key = 0;      ///< Of the note it plays or played last.
        float m_velocity = 0.0f;     ///< Of its note on.
        float m_tuning = 0.0f;       ///< Semitones away from the key.
        float m_pressure = 0.0f;     ///< 0 to 1.
        float m_timbre = 0.5f;       ///< 0 to 1, 0.5 at rest.
    };

    /// Voices voice() can show: VoiceAllocator::k_max_voices.
    static constexpr std::size_t k_max_voices = 64;

    /// `log_capacity` events for drain(), rounded up to a power of two. Allocates.
    explicit Monitor(std::size_t log_capacity = k_default_log_capacity);

    /// Takes in the events of one block (EventStream::events()).
    void observe(std::span<const Event> events) noexcept TANH_NONBLOCKING_FUNCTION;

    /// Puts all state back to rest. The log and the counters stay.
    void reset() noexcept TANH_NONBLOCKING_FUNCTION;

    /// Last value of control change `index` (0 to 127) on `channel` (0 to 15), 0 to 1.
    [[nodiscard]] float controller(std::uint8_t channel, std::uint8_t index) const noexcept {
        return m_controllers[slot(channel, index)].load(std::memory_order_relaxed);
    }
    /// Last pitch bend on `channel`, -1 to 1.
    [[nodiscard]] float pitch_bend(std::uint8_t channel) const noexcept {
        return m_pitch_bend[channel & 0xFU].load(std::memory_order_relaxed);
    }
    /// Last channel pressure on `channel`, 0 to 1.
    [[nodiscard]] float channel_pressure(std::uint8_t channel) const noexcept {
        return m_pressure[channel & 0xFU].load(std::memory_order_relaxed);
    }
    /// Velocity of `key` (0 to 127) on `channel` while it is held, 0 while it is not.
    [[nodiscard]] float note_velocity(std::uint8_t channel, std::uint8_t key) const noexcept {
        return m_notes[slot(channel, key)].load(std::memory_order_relaxed);
    }
    /// Keys held on all channels.
    [[nodiscard]] std::uint32_t held_notes() const noexcept {
        return m_held.load(std::memory_order_relaxed);
    }

    /// Voice `index` (below k_max_voices). The fields are read one by one, so a view taken
    /// while a note starts may mix the old note with the new for one read.
    [[nodiscard]] VoiceView voice(std::size_t index) const noexcept;

    /// The controller that moved last. Mode messages (CC 120 to 127) and RPNs do not count:
    /// nobody maps those.
    [[nodiscard]] LastController last_controller() const noexcept;

    /// Events seen since construction.
    [[nodiscard]] std::uint64_t event_count() const noexcept {
        return m_event_count.load(std::memory_order_relaxed);
    }

    /// Calls `sink(const Event&)` for every logged event, oldest first, and returns how
    /// many there were; at most log_capacity() per call.
    template <typename Sink>
    std::size_t drain(Sink&& sink) {
        const std::size_t limit = m_log.capacity();
        std::size_t count = 0;
        Event event;
        while (count < limit && m_log.try_pop(event)) {
            sink(static_cast<const Event&>(event));
            ++count;
        }
        return count;
    }

    /// Events left out of the log because it was full (wraps at 2^32).
    [[nodiscard]] std::uint32_t log_dropped() const noexcept {
        return m_log_dropped.load(std::memory_order_relaxed);
    }
    /// Events the log holds.
    [[nodiscard]] std::size_t log_capacity() const noexcept { return m_log.capacity(); }

    /// Log capacity of a default-constructed Monitor.
    static constexpr std::size_t k_default_log_capacity = 512;

private:
    static constexpr std::size_t k_slots = std::size_t{16} * 128;

    [[nodiscard]] static constexpr std::size_t slot(std::uint8_t channel,
                                                    std::uint8_t index) noexcept {
        return (static_cast<std::size_t>(channel & 0xFU) << 7U) | (index & 0x7FU);
    }

    struct VoiceState {
        std::atomic<std::uint32_t> m_note{0};  // active << 16 | channel << 8 | key
        std::atomic<float> m_velocity{0.0f};
        std::atomic<float> m_tuning{0.0f};
        std::atomic<float> m_pressure{0.0f};
        std::atomic<float> m_timbre{0.5f};
    };

    void note(const Event& event, bool on) noexcept;
    void moved(const Event& event) noexcept;
    void voice_event(const Event& event) noexcept;

    std::array<std::atomic<float>, k_slots> m_controllers{};
    std::array<std::atomic<float>, k_slots> m_notes{};
    std::array<std::atomic<float>, 16> m_pitch_bend{};
    std::array<std::atomic<float>, 16> m_pressure{};
    std::atomic<std::uint32_t> m_held{0};
    std::array<VoiceState, k_max_voices> m_voices{};

    // serial << 32 | type << 24 | channel << 16 | index
    std::atomic<std::uint64_t> m_last{0};
    std::atomic<float> m_last_value{0.0f};

    std::atomic<std::uint64_t> m_event_count{0};
    std::atomic<std::uint32_t> m_log_dropped{0};
    core::DynamicLockFreeQueue<Event> m_log;
};

}  // namespace thl::inline THL_ABI_NAMESPACE::midi
