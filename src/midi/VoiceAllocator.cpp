#include <tanh/core/AbiNamespace.h>
#include <tanh/midi/Event.h>
#include <tanh/midi/VoiceAllocator.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace thl::inline THL_ABI_NAMESPACE::midi {

namespace {

constexpr std::uint16_t k_cc_sustain = 64;
constexpr std::uint16_t k_cc_sostenuto = 66;
constexpr std::uint16_t k_cc_timbre = 74;
constexpr std::uint16_t k_cc_all_sound_off = 120;
constexpr std::uint16_t k_cc_reset_all_controllers = 121;
constexpr std::uint16_t k_cc_all_notes_off = 123;

constexpr std::uint16_t k_rpn_pitch_bend_range = 0;
constexpr std::uint16_t k_rpn_mpe_configuration = 6;

constexpr std::uint8_t k_lower_manager = 0;
constexpr std::uint8_t k_upper_manager = 15;
constexpr std::uint8_t k_max_members = 15;
// Member channels two zones can share: 16 channels less the two managers.
constexpr std::uint8_t k_shared_members = 14;

constexpr std::uint32_t k_zone_request_flag = 1U << 16U;

Event expression_event(const VoiceAllocator::Voice& voice,
                       std::size_t index,
                       std::uint32_t sample_offset,
                       Expression expression,
                       float value) noexcept {
    Event event;
    event.m_sample_offset = sample_offset;
    event.m_value = value;
    event.m_index = static_cast<std::uint16_t>(expression);
    event.m_source = voice.m_source;
    event.m_voice = static_cast<std::int16_t>(index);
    event.m_type = EventType::NoteExpression;
    event.m_group = voice.m_group;
    event.m_channel = voice.m_channel;
    event.m_key = voice.m_key;
    return event;
}

}  // namespace

VoiceAllocator::VoiceAllocator() = default;

void VoiceAllocator::set_voice_count(std::uint32_t count) noexcept {
    m_voice_count = std::clamp<std::uint32_t>(count, 1, k_max_voices);
    reset();
}

void VoiceAllocator::set_zones(std::uint8_t lower_members, std::uint8_t upper_members) noexcept {
    const std::uint32_t lower = std::min(lower_members, k_max_members);
    const std::uint32_t upper = std::min(upper_members, k_max_members);
    m_zone_request.store(k_zone_request_flag | (upper << 8U) | lower, std::memory_order_release);
}

void VoiceAllocator::begin_block(EventWriter& out) noexcept TANH_NONBLOCKING_FUNCTION {
    const std::uint32_t request = m_zone_request.exchange(0, std::memory_order_acquire);
    if (request == 0) { return; }
    apply_zones(static_cast<std::uint8_t>(request & 0xFFU),
                static_cast<std::uint8_t>((request >> 8U) & 0xFFU),
                /*lower_wins=*/true);
    for (std::size_t i = 0; i < m_voice_count; ++i) { update_voice(i, 0, false, out); }
}

void VoiceAllocator::process(const Event& event,
                             EventWriter& out) noexcept TANH_NONBLOCKING_FUNCTION {
    const std::uint8_t channel = event.m_channel & 0xFU;

    switch (event.m_type) {
        case EventType::NoteOn: note_on(event, out); break;
        case EventType::NoteOff: note_off(event, out); break;
        case EventType::NoteExpression: note_expression(event, out); break;
        case EventType::Controller: controller(event, out); break;
        case EventType::RegisteredController: registered_controller(event, out); break;
        case EventType::PitchBend:
            m_channels[channel].m_bend = event.m_value;
            if (!is_member(channel)) { out.push(event); }
            update_channel(channel, event.m_sample_offset, out);
            break;
        case EventType::ChannelPressure:
            m_channels[channel].m_pressure = event.m_value;
            if (!is_member(channel)) { out.push(event); }
            update_channel(channel, event.m_sample_offset, out);
            break;
        default: out.push(event); break;
    }
}

void VoiceAllocator::release_all(std::uint32_t sample_offset,
                                 EventWriter& out) noexcept TANH_NONBLOCKING_FUNCTION {
    // Every voice that ever played gets a note off, sounding here or not: a note off that
    // did not fit an earlier block left its voice sounding downstream only.
    for (std::size_t i = 0; i < m_voice_count; ++i) {
        if (m_voices[i].m_stamp != 0) { end_voice(i, sample_offset, out); }
    }
    // Whatever was lost may have been a pedal coming up: a pedal that really is down
    // says so again with its next message.
    for (auto& channel : m_channels) {
        channel.m_sustain = false;
        channel.m_sostenuto = false;
    }
}

void VoiceAllocator::reset() noexcept {
    m_voices.fill(VoiceState{});
    m_channels.fill(ChannelState{});
    m_counter = 0;
    for (std::uint8_t channel = 0; channel < 16; ++channel) {
        if (is_member(channel)) { m_channels[channel].m_bend_range = k_member_bend_range; }
    }
}

bool VoiceAllocator::is_member(std::uint8_t channel) const noexcept {
    return manager_of(channel) >= 0;
}

int VoiceAllocator::manager_of(std::uint8_t channel) const noexcept {
    const int lower = lower_zone_members();
    const int upper = upper_zone_members();
    const int index = channel;
    if (lower > 0 && index >= 1 && index <= lower) { return k_lower_manager; }
    if (upper > 0 && index <= 14 && index >= 15 - upper) { return k_upper_manager; }
    return -1;
}

bool VoiceAllocator::is_manager(std::uint8_t channel) const noexcept {
    return (channel == k_lower_manager && lower_zone_members() > 0) ||
           (channel == k_upper_manager && upper_zone_members() > 0);
}

bool VoiceAllocator::reaches(std::uint8_t channel, const VoiceState& voice) const noexcept {
    if (!voice.m_public.m_active) { return false; }
    return voice.m_public.m_channel == channel || manager_of(voice.m_public.m_channel) == channel;
}

int VoiceAllocator::find(const Event& note) const noexcept {
    for (std::size_t i = 0; i < m_voice_count; ++i) {
        const Voice& voice = m_voices[i].m_public;
        if (voice.m_active && voice.m_key == note.m_key && voice.m_channel == note.m_channel &&
            voice.m_group == note.m_group && voice.m_source == note.m_source) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::size_t VoiceAllocator::allocate() const noexcept {
    // A free voice first, then one only a pedal holds, then one whose key is down; within
    // each kind the one whose stamp is oldest (released longest ago, held longest).
    const auto rank = [](const VoiceState& voice) {
        if (!voice.m_public.m_active) { return 0; }
        return voice.m_public.m_key_down ? 2 : 1;
    };
    std::size_t best = 0;
    for (std::size_t i = 1; i < m_voice_count; ++i) {
        const auto& candidate = m_voices[i];
        const auto& current = m_voices[best];
        const int candidate_rank = rank(candidate);
        const int current_rank = rank(current);
        if (candidate_rank < current_rank ||
            (candidate_rank == current_rank && candidate.m_stamp < current.m_stamp)) {
            best = i;
        }
    }
    return best;
}

void VoiceAllocator::note_on(const Event& event, EventWriter& out) noexcept {
    const int held = find(event);
    const std::size_t index = held >= 0 ? static_cast<std::size_t>(held) : allocate();

    auto& voice = m_voices[index];
    // A key the sostenuto pedal holds stays held by it when it is struck again.
    const bool sostenuto = held >= 0 && voice.m_sostenuto;
    voice = VoiceState{};
    voice.m_sostenuto = sostenuto;
    voice.m_public.m_active = true;
    voice.m_public.m_key_down = true;
    voice.m_public.m_group = event.m_group;
    voice.m_public.m_channel = event.m_channel & 0xFU;
    voice.m_public.m_key = event.m_key;
    voice.m_public.m_source = event.m_source;
    voice.m_public.m_velocity = event.m_value;
    voice.m_stamp = ++m_counter;

    Event note = event;
    note.m_voice = static_cast<std::int16_t>(index);
    out.push(note);
    update_voice(index, event.m_sample_offset, /*force=*/true, out);
}

void VoiceAllocator::note_off(const Event& event, EventWriter& out) noexcept {
    Event note = event;
    const int index = find(event);
    if (index >= 0) {
        auto& voice = m_voices[static_cast<std::size_t>(index)];
        voice.m_public.m_key_down = false;
        // With a pedal down the key's note off passes without a voice and the voice
        // sounds on; its own note off follows when the pedal lets go.
        if (!held_by_pedal(voice)) {
            voice.m_public.m_active = false;
            voice.m_stamp = ++m_counter;
            note.m_voice = static_cast<std::int16_t>(index);
        }
    }
    out.push(note);
}

void VoiceAllocator::note_expression(const Event& event, EventWriter& out) noexcept {
    const int index = find(event);
    if (index < 0) { return; }
    auto& voice = m_voices[static_cast<std::size_t>(index)];

    switch (event.expression()) {
        case Expression::Pressure: voice.m_poly_pressure = event.m_value; break;
        case Expression::Timbre: voice.m_note_timbre = event.m_value; break;
        case Expression::PitchBend: voice.m_note_bend = event.m_value; break;
        case Expression::Pitch:
            voice.m_pitch_offset = event.m_value - static_cast<float>(event.m_key);
            break;
        default: return;
    }
    update_voice(static_cast<std::size_t>(index), event.m_sample_offset, /*force=*/false, out);
}

void VoiceAllocator::controller(const Event& event, EventWriter& out) noexcept {
    const std::uint8_t channel = event.m_channel & 0xFU;

    switch (event.m_index) {
        case k_cc_timbre:
            m_channels[channel].m_timbre = event.m_value;
            if (!is_member(channel)) { out.push(event); }
            update_channel(channel, event.m_sample_offset, out);
            break;
        case k_cc_sustain:
            out.push(event);
            m_channels[channel].m_sustain = event.m_value >= 0.5f;
            release_unheld(channel, event.m_sample_offset, out);
            break;
        case k_cc_sostenuto: {
            out.push(event);
            const bool down = event.m_value >= 0.5f;
            // Going down, the pedal takes the notes whose keys are down now; held or coming
            // up, it takes no new ones.
            const bool pressed = down && !m_channels[channel].m_sostenuto;
            m_channels[channel].m_sostenuto = down;
            for (std::size_t i = 0; i < m_voice_count; ++i) {
                auto& voice = m_voices[i];
                if (!reaches(channel, voice)) { continue; }
                if (pressed && voice.m_public.m_key_down) { voice.m_sostenuto = true; }
                if (!down) { voice.m_sostenuto = false; }
            }
            release_unheld(channel, event.m_sample_offset, out);
            break;
        }
        case k_cc_all_sound_off:
        case k_cc_all_notes_off:
            out.push(event);
            release_channel(channel, event.m_sample_offset, out);
            break;
        case k_cc_reset_all_controllers: {
            out.push(event);
            auto& state = m_channels[channel];
            state.m_bend = 0.0f;
            state.m_pressure = 0.0f;
            state.m_timbre = 0.5f;
            state.m_sustain = false;
            state.m_sostenuto = false;
            for (std::size_t i = 0; i < m_voice_count; ++i) {
                if (reaches(channel, m_voices[i])) { m_voices[i].m_sostenuto = false; }
            }
            update_channel(channel, event.m_sample_offset, out);
            release_unheld(channel, event.m_sample_offset, out);
            break;
        }
        default: out.push(event); break;
    }
}

void VoiceAllocator::registered_controller(const Event& event, EventWriter& out) noexcept {
    const std::uint8_t channel = event.m_channel & 0xFU;
    out.push(event);

    if (event.m_index == k_rpn_pitch_bend_range) {
        // Data entry MSB: semitones, LSB: cents.
        const float semitones = static_cast<float>(event.m_data >> 25U) +
                                static_cast<float>((event.m_data >> 18U) & 0x7FU) * 0.01f;
        const int manager = manager_of(channel);
        if (manager < 0) {
            m_channels[channel].m_bend_range = semitones;
        } else {
            // The members of a zone share one range: the last one any of them received.
            for (std::uint8_t other = 0; other < 16; ++other) {
                if (manager_of(other) == manager) { m_channels[other].m_bend_range = semitones; }
            }
        }
    } else if (event.m_index == k_rpn_mpe_configuration &&
               m_follow_configuration.load(std::memory_order_relaxed) &&
               (channel == k_lower_manager || channel == k_upper_manager)) {
        const auto members =
            static_cast<std::uint8_t>(std::min<std::uint32_t>(event.m_data >> 25U, k_max_members));
        if (channel == k_lower_manager) {
            apply_zones(members, upper_zone_members(), /*lower_wins=*/true);
        } else {
            apply_zones(lower_zone_members(), members, /*lower_wins=*/false);
        }
    } else {
        return;
    }
    for (std::size_t i = 0; i < m_voice_count; ++i) {
        update_voice(i, event.m_sample_offset, false, out);
    }
}

void VoiceAllocator::apply_zones(std::uint8_t lower, std::uint8_t upper, bool lower_wins) noexcept {
    // A channel belongs to one zone: where two zones that are both on overlap, the newer
    // one keeps its members and the other shrinks, down to nothing when the newer one
    // takes its manager channel too. A zone alone may have all 15.
    if (lower > 0 && upper > 0 && lower + upper > k_shared_members) {
        if (lower_wins) {
            upper = lower >= k_shared_members ? std::uint8_t{0}
                                              : static_cast<std::uint8_t>(k_shared_members - lower);
        } else {
            lower = upper >= k_shared_members ? std::uint8_t{0}
                                              : static_cast<std::uint8_t>(k_shared_members - upper);
        }
    }
    m_lower_members.store(lower, std::memory_order_relaxed);
    m_upper_members.store(upper, std::memory_order_relaxed);

    for (std::uint8_t channel = 0; channel < 16; ++channel) {
        m_channels[channel].m_bend_range =
            is_member(channel) ? k_member_bend_range : k_channel_bend_range;
    }
}

void VoiceAllocator::release_channel(std::uint8_t channel,
                                     std::uint32_t sample_offset,
                                     EventWriter& out) noexcept {
    for (std::size_t i = 0; i < m_voice_count; ++i) {
        if (reaches(channel, m_voices[i])) { end_voice(i, sample_offset, out); }
    }
}

bool VoiceAllocator::held_by_pedal(const VoiceState& voice) const noexcept {
    if (!m_follow_pedals.load(std::memory_order_relaxed)) { return false; }
    if (voice.m_sostenuto) { return true; }
    if (m_channels[voice.m_public.m_channel].m_sustain) { return true; }
    const int manager = manager_of(voice.m_public.m_channel);
    return manager >= 0 && m_channels[static_cast<std::size_t>(manager)].m_sustain;
}

void VoiceAllocator::release_unheld(std::uint8_t channel,
                                    std::uint32_t sample_offset,
                                    EventWriter& out) noexcept {
    for (std::size_t i = 0; i < m_voice_count; ++i) {
        const auto& voice = m_voices[i];
        if (reaches(channel, voice) && !voice.m_public.m_key_down && !held_by_pedal(voice)) {
            end_voice(i, sample_offset, out);
        }
    }
}

void VoiceAllocator::end_voice(std::size_t index,
                               std::uint32_t sample_offset,
                               EventWriter& out) noexcept {
    auto& voice = m_voices[index];
    Event event =
        expression_event(voice.m_public, index, sample_offset, Expression::Pressure, 0.0f);
    event.m_type = EventType::NoteOff;
    event.m_index = 0;
    out.push(event);
    voice.m_public.m_active = false;
    voice.m_public.m_key_down = false;
    voice.m_sostenuto = false;
    voice.m_stamp = ++m_counter;
}

void VoiceAllocator::update_channel(std::uint8_t channel,
                                    std::uint32_t sample_offset,
                                    EventWriter& out) noexcept {
    for (std::size_t i = 0; i < m_voice_count; ++i) {
        if (reaches(channel, m_voices[i])) { update_voice(i, sample_offset, false, out); }
    }
}

void VoiceAllocator::update_voice(std::size_t index,
                                  std::uint32_t sample_offset,
                                  bool force,
                                  EventWriter& out) noexcept {
    auto& voice = m_voices[index];
    Voice& state = voice.m_public;
    if (!state.m_active) { return; }

    const ChannelState& channel = m_channels[state.m_channel];
    float tuning = voice.m_pitch_offset + voice.m_note_bend * per_note_bend_range() +
                   channel.m_bend * channel.m_bend_range;
    float pressure = std::max(voice.m_poly_pressure, channel.m_pressure);
    float timbre = 0.5f;

    const int manager = manager_of(state.m_channel);
    if (manager >= 0) {
        const ChannelState& zone = m_channels[static_cast<std::size_t>(manager)];
        tuning += zone.m_bend * zone.m_bend_range;
        pressure = std::max(pressure, zone.m_pressure);
        timbre = std::clamp(channel.m_timbre + zone.m_timbre - 0.5f, 0.0f, 1.0f);
    } else if (is_manager(state.m_channel)) {
        timbre = channel.m_timbre;
    }
    if (voice.m_note_timbre >= 0.0f) { timbre = voice.m_note_timbre; }

    const auto send = [&](Expression expression, float& last, float value) {
        if (!force && last == value) { return; }
        last = value;
        out.push(expression_event(state, index, sample_offset, expression, value));
    };
    send(Expression::Tuning, state.m_tuning, tuning);
    send(Expression::Pressure, state.m_pressure, pressure);
    send(Expression::Timbre, state.m_timbre, timbre);
}

}  // namespace thl::inline THL_ABI_NAMESPACE::midi
