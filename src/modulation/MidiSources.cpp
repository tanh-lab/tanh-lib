#include <tanh/core/AbiNamespace.h>
#include <tanh/midi/Event.h>
#include <tanh/midi/EventStream.h>
#include <tanh/modulation/MidiSources.h>
#include <tanh/modulation/ModulationSource.h>
#include <tanh/parameters/ModulationScope.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace thl::inline THL_ABI_NAMESPACE::modulation {

namespace {

// type << 24 | (channel + 1) << 16 | index; channel + 1 is 0 for "any channel".
std::uint32_t pack(const MidiControllerAddress& address) noexcept {
    const bool indexed = address.m_type == midi::EventType::Controller ||
                         address.m_type == midi::EventType::AssignableController;
    const int channel = address.m_channel < 0 ? 0 : (address.m_channel & 0xF) + 1;
    return (static_cast<std::uint32_t>(address.m_type) << 24U) |
           (static_cast<std::uint32_t>(channel) << 16U) |
           (indexed ? static_cast<std::uint32_t>(address.m_index) : 0U);
}

MidiControllerAddress unpack(std::uint32_t packed) noexcept {
    const int channel = static_cast<int>((packed >> 16U) & 0xFFU);
    return {.m_type = static_cast<midi::EventType>(packed >> 24U),
            .m_index = static_cast<std::uint16_t>(packed & 0xFFFFU),
            .m_channel = static_cast<std::int8_t>(channel - 1)};
}

bool matches(std::uint32_t packed, const midi::Event& event) noexcept {
    const MidiControllerAddress address = unpack(packed);
    if (event.m_type != address.m_type) { return false; }
    if (address.m_channel >= 0 && (event.m_channel & 0xFU) != address.m_channel) { return false; }
    const bool indexed = address.m_type == midi::EventType::Controller ||
                         address.m_type == midi::EventType::AssignableController;
    return !indexed || event.m_index == address.m_index;
}

// True when `offset` is not among `points` yet. A source records each sample once, so its
// change point list never outgrows the block it was reserved for, however many events a
// block brings.
bool is_new_change_point(const std::vector<uint32_t>& points, std::uint32_t offset) noexcept {
    return std::ranges::find(points, offset) == points.end();
}

// The binding word of a MidiControllerSource (see MidiSources.h).
constexpr std::uint64_t k_pickup_armed = 1ULL << 16U;
constexpr std::uint64_t k_serial_step = 1ULL << 17U;
constexpr std::uint64_t k_request_mask = 0xFFFFFFFFULL;  // serial, armed flag and position

std::uint32_t address_of(std::uint64_t binding) noexcept {
    return static_cast<std::uint32_t>(binding >> 32U);
}

// A pickup position, -1 to 1, in 16 bits.
std::uint64_t quantize(float position) noexcept {
    const float unit = (std::clamp(position, -1.0f, 1.0f) + 1.0f) * 0.5f;
    return static_cast<std::uint64_t>(std::lround(unit * 65535.0f));
}

float pickup_position_of(std::uint64_t binding) noexcept {
    return static_cast<float>(binding & 0xFFFFU) / 65535.0f * 2.0f - 1.0f;
}

// The next request word: a new serial, so that asking twice for the same thing is seen.
std::uint64_t next_request(std::uint64_t binding, std::uint64_t request) noexcept {
    const std::uint64_t serial =
        ((binding & k_request_mask) & ~(k_serial_step - 1)) + k_serial_step;
    return (serial & k_request_mask) | request;
}

// Longest glide a source renders, in samples.
constexpr double k_max_glide_samples = 1.0e9;

std::uint32_t offset_in_block(const midi::Event& event, size_t block_size) noexcept {
    return std::min(event.m_sample_offset, static_cast<std::uint32_t>(block_size - 1));
}

}  // namespace

// ── MidiControllerSource ────────────────────────────────────────────────────

MidiControllerSource::MidiControllerSource(const midi::EventStream& stream,
                                           MidiControllerAddress address,
                                           MidiPolarity polarity)
    : ModulationSource(k_global_scope, /*fully_active=*/false)
    , m_stream(&stream)
    , m_polarity(polarity)
    , m_binding(static_cast<std::uint64_t>(pack(address)) << 32U)
    , m_bound(static_cast<std::uint64_t>(pack(address)) << 32U) {}

void MidiControllerSource::prepare(double sample_rate,
                                   size_t samples_per_block,
                                   uint32_t voice_count) {
    resize_buffers(samples_per_block, voice_count);
    m_sample_rate = sample_rate;
    // A glide does not survive a new stream: the output is where it was going.
    if (m_glide_left != 0) { m_current = m_target; }
    m_glide_left = 0;
    std::ranges::fill(m_output_buffer, m_current);
    std::ranges::fill(get_output_active(), static_cast<std::uint8_t>(m_active));
}

void MidiControllerSource::pre_process_block() {
    const size_t bs = block_size();
    if (bs == 0) { return; }
    // The block is as long as the stream says, never longer than prepared for.
    const auto end = static_cast<std::uint32_t>(std::min(m_stream->num_samples(), bs));

    // What the other threads asked for: another controller, a pickup, or both at once.
    const std::uint64_t binding = m_binding.load(std::memory_order_acquire);
    const std::uint32_t address = address_of(binding);
    if (binding != m_bound) {
        const bool new_address = address != address_of(m_bound);
        const bool new_request = (binding & k_request_mask) != (m_bound & k_request_mask);
        m_bound = binding;

        bool restart = new_address;
        if (new_request) {
            if ((binding & k_pickup_armed) != 0) {
                m_pickup_position = pickup_position_of(binding);
                m_pickup = Pickup::FirstLook;
                restart = true;
            } else {
                m_pickup = Pickup::None;
            }
        } else if (m_pickup != Pickup::None) {
            // A pickup still waiting looks at the new controller afresh.
            m_pickup = Pickup::FirstLook;
        }
        if (restart) {
            if (m_active) { mark(0); }
            m_active = false;
            m_current = 0.0f;
            m_glide_left = 0;
        }
    }

    std::uint32_t position = 0;
    for (const midi::Event& event : m_stream->events()) {
        if (end == 0) { break; }
        if (!matches(address, event)) { continue; }
        const float value = output_of(event);
        if (!m_active && !picks_up(value)) { continue; }

        const std::uint32_t offset = std::max(offset_in_block(event, end), position);
        render(position, offset);
        position = offset;

        // Samples the glide takes; anything that is not a positive time steps.
        const double samples = static_cast<double>(smoothing_ms()) * 0.001 * m_sample_rate;
        const auto glide =
            samples > 0.0 ? static_cast<std::uint32_t>(std::min(samples, k_max_glide_samples)) : 0U;
        m_target = value;
        if (!m_active || glide == 0) {
            m_active = true;
            m_current = value;
            m_glide_left = 0;
        } else {
            m_glide_left = glide;
            m_step = (value - m_current) / static_cast<float>(glide);
            m_since_mark = 0;
        }
        mark(offset);
    }
    render(position, end);

    m_last_output = m_current;
    m_value.store(m_current, std::memory_order_relaxed);
    m_waiting.store(m_pickup != Pickup::None, std::memory_order_relaxed);
}

void MidiControllerSource::render(std::uint32_t from, std::uint32_t to) {
    auto& active = get_output_active();
    const auto flag = static_cast<std::uint8_t>(m_active);
    for (std::uint32_t i = from; i < to; ++i) {
        if (m_glide_left != 0) {
            --m_glide_left;
            m_current = m_glide_left == 0 ? m_target : m_current + m_step;
            if (++m_since_mark >= k_glide_change_point_interval || m_glide_left == 0) {
                m_since_mark = 0;
                mark(i);
            }
        }
        m_output_buffer[i] = m_current;
        active[i] = flag;
    }
}

void MidiControllerSource::mark(std::uint32_t offset) {
    if (is_new_change_point(m_change_points, offset)) { record_change_point(offset); }
}

bool MidiControllerSource::picks_up(float value) noexcept {
    const float window =
        m_polarity == MidiPolarity::Bipolar ? 2.0f * k_pickup_window : k_pickup_window;
    bool reached = false;
    switch (m_pickup) {
        case Pickup::None: return true;
        case Pickup::FirstLook: reached = std::abs(value - m_pickup_position) <= window; break;
        case Pickup::Below: reached = value >= m_pickup_position - window; break;
        case Pickup::Above: reached = value <= m_pickup_position + window; break;
    }
    m_pickup = reached ? Pickup::None : (value < m_pickup_position ? Pickup::Below : Pickup::Above);
    return reached;
}

void MidiControllerSource::arm_pickup(float position) noexcept {
    std::uint64_t binding = m_binding.load(std::memory_order_relaxed);
    std::uint64_t wanted = 0;
    do {
        wanted = (binding & ~k_request_mask) |
                 next_request(binding, k_pickup_armed | quantize(position));
    } while (!m_binding.compare_exchange_weak(binding, wanted, std::memory_order_release));
    m_waiting.store(true, std::memory_order_relaxed);
}

void MidiControllerSource::disarm_pickup() noexcept {
    std::uint64_t binding = m_binding.load(std::memory_order_relaxed);
    std::uint64_t wanted = 0;
    do {
        wanted = (binding & ~k_request_mask) | next_request(binding, 0);
    } while (!m_binding.compare_exchange_weak(binding, wanted, std::memory_order_release));
    m_waiting.store(false, std::memory_order_relaxed);
}

void MidiControllerSource::bind(MidiControllerAddress address, float pickup_position) noexcept {
    std::uint64_t binding = m_binding.load(std::memory_order_relaxed);
    std::uint64_t wanted = 0;
    do {
        wanted = (static_cast<std::uint64_t>(pack(address)) << 32U) |
                 next_request(binding, k_pickup_armed | quantize(pickup_position));
    } while (!m_binding.compare_exchange_weak(binding, wanted, std::memory_order_release));
    m_waiting.store(true, std::memory_order_relaxed);
}

void MidiControllerSource::set_address(MidiControllerAddress address) noexcept {
    std::uint64_t binding = m_binding.load(std::memory_order_relaxed);
    std::uint64_t wanted = 0;
    do {
        wanted = (static_cast<std::uint64_t>(pack(address)) << 32U) | (binding & k_request_mask);
    } while (!m_binding.compare_exchange_weak(binding, wanted, std::memory_order_release));
}

MidiControllerAddress MidiControllerSource::address() const noexcept {
    return unpack(address_of(m_binding.load(std::memory_order_relaxed)));
}

float MidiControllerSource::output_of(const midi::Event& event) const noexcept {
    const bool bend = event.m_type == midi::EventType::PitchBend;
    if (m_polarity == MidiPolarity::Bipolar) {
        return bend ? event.m_value : event.m_value * 2.0f - 1.0f;
    }
    return bend ? (event.m_value + 1.0f) * 0.5f : event.m_value;
}

// ── MidiNoteSource ──────────────────────────────────────────────────────────

MidiNoteSource::MidiNoteSource(const midi::EventStream& stream,
                               MidiNoteStream what,
                               ModulationScope scope)
    : ModulationSource(scope, /*fully_active=*/false), m_stream(&stream), m_what(what) {}

void MidiNoteSource::prepare(double /*sample_rate*/,
                             size_t samples_per_block,
                             uint32_t voice_count) {
    resize_buffers(samples_per_block, voice_count);
    m_voice_states.assign(num_voices(), VoiceState{});
}

void MidiNoteSource::set_pitch_range(float low, float high) noexcept {
    m_pitch_low.store(low, std::memory_order_relaxed);
    m_pitch_high.store(high, std::memory_order_relaxed);
}

float MidiNoteSource::pitch_value(const VoiceState& voice) const noexcept {
    const float low = m_pitch_low.load(std::memory_order_relaxed);
    const float high = m_pitch_high.load(std::memory_order_relaxed);
    if (!(high > low)) { return 0.0f; }
    return std::clamp((voice.m_key + voice.m_tuning - low) / (high - low), 0.0f, 1.0f);
}

void MidiNoteSource::pre_process_block() {
    const size_t bs = block_size();
    if (bs == 0) { return; }
    // The block is as long as the stream says, never longer than prepared for.
    const size_t length = std::min(m_stream->num_samples(), bs);
    const uint32_t voices = num_voices();

    const auto write = [this, bs](uint32_t v, std::uint32_t from, const VoiceState& state) {
        std::fill(voice_output(v) + from, voice_output(v) + bs, state.m_value);
        std::fill(voice_output_active(v) + from,
                  voice_output_active(v) + bs,
                  static_cast<std::uint8_t>(state.m_active));
        if (is_new_change_point(get_voice_change_points(v), from)) {
            record_voice_change_point(v, from);
        }
    };

    for (uint32_t v = 0; v < voices; ++v) {
        VoiceState& state = m_voice_states[v];
        std::fill_n(voice_output(v), bs, state.m_value);
        std::fill_n(voice_output_active(v), bs, static_cast<std::uint8_t>(state.m_active));
        if (state.m_reopen) {
            state.m_reopen = false;
            state.m_active = true;
            state.m_value = 1.0f;
            write(v, 0, state);
        }
    }

    for (const midi::Event& event : m_stream->events()) {
        if (event.m_voice < 0) { continue; }
        const auto v = static_cast<uint32_t>(event.m_voice);
        if (v >= voices) { continue; }
        if (length == 0) { break; }
        const std::uint32_t offset = offset_in_block(event, length);
        VoiceState& state = m_voice_states[v];

        switch (event.m_type) {
            case midi::EventType::NoteOn:
                if (m_what == MidiNoteStream::Gate && state.m_active) {
                    // The voice still sounds: close the gate for this one sample.
                    state.m_active = false;
                    state.m_value = 0.0f;
                    write(v, offset, state);
                    if (offset + 1 == length) {
                        state.m_reopen = true;
                        continue;
                    }
                    state.m_active = true;
                    state.m_value = 1.0f;
                    write(v, offset + 1, state);
                    continue;
                }
                state.m_active = true;
                if (m_what == MidiNoteStream::Gate) {
                    state.m_value = 1.0f;
                } else if (m_what == MidiNoteStream::Velocity) {
                    state.m_value = event.m_value;
                } else if (m_what == MidiNoteStream::Pitch) {
                    state.m_key = static_cast<float>(event.m_key);
                    state.m_tuning = 0.0f;
                    state.m_value = pitch_value(state);
                }
                break;
            case midi::EventType::NoteOff:
                state.m_active = false;
                state.m_reopen = false;
                if (m_what == MidiNoteStream::Gate) { state.m_value = 0.0f; }
                break;
            case midi::EventType::NoteExpression:
                if (m_what == MidiNoteStream::Pitch &&
                    event.expression() == midi::Expression::Tuning) {
                    state.m_tuning = event.m_value;
                    state.m_value = pitch_value(state);
                } else if ((m_what == MidiNoteStream::Pressure &&
                            event.expression() == midi::Expression::Pressure) ||
                           (m_what == MidiNoteStream::Timbre &&
                            event.expression() == midi::Expression::Timbre)) {
                    state.m_value = event.m_value;
                } else {
                    continue;
                }
                break;
            default: continue;
        }
        write(v, offset, state);
    }
}

}  // namespace thl::inline THL_ABI_NAMESPACE::modulation
