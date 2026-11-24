#include <tanh/core/AbiNamespace.h>
#include <tanh/midi/Event.h>
#include <tanh/midi/Monitor.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace thl::inline THL_ABI_NAMESPACE::midi {

namespace {

constexpr std::uint16_t k_cc_all_sound_off = 120;
constexpr std::uint16_t k_cc_all_notes_off = 123;
constexpr std::uint16_t k_first_mode_message = 120;
constexpr std::uint32_t k_voice_active = 1U << 16U;

}  // namespace

Monitor::Monitor(std::size_t log_capacity) : m_log(log_capacity) {}

void Monitor::observe(std::span<const Event> events) noexcept TANH_NONBLOCKING_FUNCTION {
    for (const Event& event : events) {
        const std::uint8_t channel = event.m_channel & 0xFU;

        switch (event.m_type) {
            case EventType::NoteOn: note(event, true); break;
            case EventType::NoteOff: note(event, false); break;
            case EventType::Controller: {
                const auto index = static_cast<std::uint8_t>(event.m_index & 0x7FU);
                m_controllers[slot(channel, index)].store(event.m_value, std::memory_order_relaxed);
                if (event.m_index == k_cc_all_sound_off || event.m_index == k_cc_all_notes_off) {
                    for (std::uint8_t key = 0; key < 128; ++key) {
                        Event off = event;
                        off.m_key = key;
                        note(off, false);
                    }
                }
                if (event.m_index < k_first_mode_message) { moved(event); }
                break;
            }
            case EventType::AssignableController: moved(event); break;
            case EventType::PitchBend:
                m_pitch_bend[channel].store(event.m_value, std::memory_order_relaxed);
                moved(event);
                break;
            case EventType::ChannelPressure:
                m_pressure[channel].store(event.m_value, std::memory_order_relaxed);
                moved(event);
                break;
            default: break;
        }

        if (event.m_voice >= 0) { voice_event(event); }
        if (!m_log.try_push(event)) { m_log_dropped.fetch_add(1, std::memory_order_relaxed); }
    }
    m_event_count.fetch_add(events.size(), std::memory_order_relaxed);
}

void Monitor::reset() noexcept TANH_NONBLOCKING_FUNCTION {
    for (auto& value : m_controllers) { value.store(0.0f, std::memory_order_relaxed); }
    for (auto& value : m_notes) { value.store(0.0f, std::memory_order_relaxed); }
    for (auto& value : m_pitch_bend) { value.store(0.0f, std::memory_order_relaxed); }
    for (auto& value : m_pressure) { value.store(0.0f, std::memory_order_relaxed); }
    m_held.store(0, std::memory_order_relaxed);
    for (auto& voice : m_voices) {
        voice.m_note.store(0, std::memory_order_relaxed);
        voice.m_velocity.store(0.0f, std::memory_order_relaxed);
        voice.m_tuning.store(0.0f, std::memory_order_relaxed);
        voice.m_pressure.store(0.0f, std::memory_order_relaxed);
        voice.m_timbre.store(0.5f, std::memory_order_relaxed);
    }
}

Monitor::VoiceView Monitor::voice(std::size_t index) const noexcept {
    const VoiceState& state = m_voices[index];
    const std::uint32_t note = state.m_note.load(std::memory_order_relaxed);
    return {.m_active = (note & k_voice_active) != 0,
            .m_channel = static_cast<std::uint8_t>((note >> 8U) & 0xFU),
            .m_key = static_cast<std::uint8_t>(note & 0x7FU),
            .m_velocity = state.m_velocity.load(std::memory_order_relaxed),
            .m_tuning = state.m_tuning.load(std::memory_order_relaxed),
            .m_pressure = state.m_pressure.load(std::memory_order_relaxed),
            .m_timbre = state.m_timbre.load(std::memory_order_relaxed)};
}

void Monitor::voice_event(const Event& event) noexcept {
    const auto index = static_cast<std::size_t>(event.m_voice);
    if (index >= k_max_voices) { return; }
    VoiceState& state = m_voices[index];
    const std::uint32_t note =
        (static_cast<std::uint32_t>(event.m_channel & 0xFU) << 8U) | (event.m_key & 0x7FU);

    switch (event.m_type) {
        case EventType::NoteOn:
            state.m_velocity.store(event.m_value, std::memory_order_relaxed);
            state.m_note.store(k_voice_active | note, std::memory_order_relaxed);
            break;
        case EventType::NoteOff: state.m_note.store(note, std::memory_order_relaxed); break;
        case EventType::NoteExpression:
            switch (event.expression()) {
                case Expression::Tuning:
                    state.m_tuning.store(event.m_value, std::memory_order_relaxed);
                    break;
                case Expression::Pressure:
                    state.m_pressure.store(event.m_value, std::memory_order_relaxed);
                    break;
                case Expression::Timbre:
                    state.m_timbre.store(event.m_value, std::memory_order_relaxed);
                    break;
                default: break;
            }
            break;
        default: break;
    }
}

Monitor::LastController Monitor::last_controller() const noexcept {
    const std::uint64_t packed = m_last.load(std::memory_order_acquire);
    LastController last;
    last.m_serial = static_cast<std::uint32_t>(packed >> 32U);
    last.m_type = static_cast<EventType>((packed >> 24U) & 0xFFU);
    last.m_channel = static_cast<std::uint8_t>((packed >> 16U) & 0xFFU);
    last.m_index = static_cast<std::uint16_t>(packed & 0xFFFFU);
    last.m_value = m_last_value.load(std::memory_order_relaxed);
    return last;
}

void Monitor::note(const Event& event, bool on) noexcept {
    auto& held = m_notes[slot(event.m_channel, event.m_key)];
    const bool was_on = held.load(std::memory_order_relaxed) > 0.0f;
    // A held key reads above 0 even when its velocity was 0 (possible in MIDI 2.0).
    const float velocity = on ? std::max(event.m_value, std::numeric_limits<float>::min()) : 0.0f;
    held.store(velocity, std::memory_order_relaxed);
    if (on && !was_on) { m_held.fetch_add(1, std::memory_order_relaxed); }
    if (!on && was_on) { m_held.fetch_sub(1, std::memory_order_relaxed); }
}

void Monitor::moved(const Event& event) noexcept {
    const bool indexed =
        event.m_type == EventType::Controller || event.m_type == EventType::AssignableController;
    const std::uint64_t serial = (m_last.load(std::memory_order_relaxed) >> 32U) + 1;
    m_last_value.store(event.m_value, std::memory_order_relaxed);
    m_last.store((serial << 32U) | (static_cast<std::uint64_t>(event.m_type) << 24U) |
                     (static_cast<std::uint64_t>(event.m_channel & 0xFU) << 16U) |
                     (indexed ? event.m_index : std::uint64_t{0}),
                 std::memory_order_release);
}

}  // namespace thl::inline THL_ABI_NAMESPACE::midi
