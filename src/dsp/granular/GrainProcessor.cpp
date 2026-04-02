#include <tanh/dsp/granular/GrainProcessor.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>

namespace thl::dsp::granular {

GrainProcessorImpl::GrainProcessorImpl(audio::AudioDataStore& audio_store)
    : m_audio_store(audio_store)
    , m_max_grains(k_max_grains)
    , m_next_grain_time(0)
    , m_min_grain_interval(100)
    , m_sequential_position(0)
    , m_random_generator(std::random_device{}())
    , m_uni_dist(0.0f, 1.0f)
    , m_last_playing_state(false)
    , m_current_sample_index(0) {
    // Prepare grain container
    m_grains.resize(m_max_grains);

    // Initialize all grains as inactive
    for (auto& grain : m_grains) { grain.m_active = false; }
}

GrainProcessorImpl::~GrainProcessorImpl() = default;

void GrainProcessorImpl::set_visualization_listener(GrainVisualizationListener* listener) {
    m_viz_listeners.clear();
    if (listener) { m_viz_listeners.push_back(listener); }
}

void GrainProcessorImpl::add_visualization_listener(GrainVisualizationListener* listener) {
    if (listener) { m_viz_listeners.push_back(listener); }
}

void GrainProcessorImpl::remove_visualization_listener(GrainVisualizationListener* listener) {
    m_viz_listeners.erase(std::remove(m_viz_listeners.begin(), m_viz_listeners.end(), listener),
                          m_viz_listeners.end());
}

void GrainProcessorImpl::set_visualization_update_rate(float fps) {
    if (fps > 0.f && m_sample_rate > 0) {
        m_viz_update_interval = static_cast<size_t>(m_sample_rate / fps);
    } else {
        m_viz_update_interval = 0;
    }
}

void GrainProcessorImpl::reset_grains() {
    for (auto& grain : m_grains) { grain.m_active = false; }
    m_sequential_position = 0;
    m_next_grain_time = 0;
    m_last_playing_state = false;
    m_playback_elapsed_samples = 0;
    m_envelope.reset();
}

void GrainProcessorImpl::prepare(const double& sample_rate,
                                 const size_t& samples_per_block,
                                 const size_t& num_channels) {
    m_sample_rate = sample_rate;
    m_channels = num_channels;

    for (auto& grain : m_grains) {
        grain.m_envelope.set_sample_rate(static_cast<float>(m_sample_rate));
    }
    m_next_grain_time = 0;

    m_envelope.set_sample_rate(static_cast<float>(m_sample_rate));
    m_envelope.set_parameters(get_parameter<float>(EnvelopeAttack),
                              get_parameter<float>(EnvelopeDecay),
                              get_parameter<float>(EnvelopeSustain),
                              get_parameter<float>(EnvelopeRelease),
                              get_parameter<float>(EnvelopeAttackCurve),
                              get_parameter<float>(EnvelopeDecayCurve),
                              get_parameter<float>(EnvelopeReleaseCurve));
    m_envelope.reset();
}

void GrainProcessorImpl::process(thl::dsp::audio::AudioBufferView buffer,
                                 uint32_t modulation_offset) {
    const size_t num_samples = buffer.get_num_frames();
    const size_t num_channels =
        std::min(buffer.get_num_channels(), static_cast<size_t>(k_max_channel_support));
    float* channel_ptrs[k_max_channel_support];
    for (size_t ch = 0; ch < num_channels; ++ch) {
        channel_ptrs[ch] = buffer.get_write_pointer(ch);
    }

    update_envelope_if_needed(modulation_offset);

    bool playing = get_parameter<bool>(Playing, modulation_offset);

    bool envelope_active = m_envelope.is_active();
    if (playing && !envelope_active || playing && !m_last_playing_state) {
        m_envelope.note_on();
        m_next_grain_time = 0;      // Reset grain time when starting playback
        m_sequential_position = 0;  // Reset sequential position when starting
                                    // playback
        m_playback_elapsed_samples = 0;
    } else if (!playing && m_envelope.get_state() != utils::ADSR::State::IDLE &&
               m_envelope.get_state() != utils::ADSR::State::RELEASE) {
        m_envelope.note_off();
    }
    m_last_playing_state = playing;

    float volume = get_parameter<float>(Volume, modulation_offset);

    // Clear the buffer
    for (size_t ch = 0; ch < num_channels; ++ch) {
        std::memset(channel_ptrs[ch], 0, num_samples * sizeof(float));
    }

    // If not playing or no audio data, just return (silence)
    if (!m_envelope.is_active() || !m_audio_store.is_loaded()) {
        // Deactivate any lingering grains and notify visualization
        if (!m_viz_listeners.empty()) {
            for (size_t gi = 0; gi < m_grains.size(); ++gi) {
                if (m_grains[gi].m_active) {
                    m_grains[gi].m_active = false;
                    for (auto* l : m_viz_listeners) { l->on_grain_finished(static_cast<int>(gi)); }
                }
            }
            for (auto* l : m_viz_listeners) { l->on_master_envelope_updated(0.f); }
        }
        return;
    }

    // Process existing grains and generate new ones
    update_grains(channel_ptrs, num_samples, modulation_offset);

    // Apply master volume
    for (size_t i = 0; i < num_samples; i++) {
        float grain_volume = volume * m_envelope.process();
        for (size_t ch = 0; ch < num_channels; ++ch) { channel_ptrs[ch][i] *= grain_volume; }
    }
}

void GrainProcessorImpl::update_envelope_if_needed(uint32_t modulation_offset) {
    float attack = get_parameter<float>(EnvelopeAttack, modulation_offset);
    float decay = get_parameter<float>(EnvelopeDecay, modulation_offset);
    float sustain = get_parameter<float>(EnvelopeSustain, modulation_offset);
    float release = get_parameter<float>(EnvelopeRelease, modulation_offset);

    if (attack != m_last_envelope_attack) {
        m_envelope.set_attack(attack);
        m_last_envelope_attack = attack;
    }
    if (decay != m_last_envelope_decay) {
        m_envelope.set_decay(decay);
        m_last_envelope_decay = decay;
    }
    if (sustain != m_last_envelope_sustain) {
        m_envelope.set_sustain(sustain);
        m_last_envelope_sustain = sustain;
    }
    if (release != m_last_envelope_release) {
        m_envelope.set_release(release);
        m_last_envelope_release = release;
    }

    float attack_curve = get_parameter<float>(EnvelopeAttackCurve, modulation_offset);
    float decay_curve = get_parameter<float>(EnvelopeDecayCurve, modulation_offset);
    float release_curve = get_parameter<float>(EnvelopeReleaseCurve, modulation_offset);

    if (attack_curve != m_last_envelope_attack_curve) {
        m_envelope.set_attack_curve(attack_curve);
        m_last_envelope_attack_curve = attack_curve;
    }
    if (decay_curve != m_last_envelope_decay_curve) {
        m_envelope.set_decay_curve(decay_curve);
        m_last_envelope_decay_curve = decay_curve;
    }
    if (release_curve != m_last_envelope_release_curve) {
        m_envelope.set_release_curve(release_curve);
        m_last_envelope_release_curve = release_curve;
    }
}

void GrainProcessorImpl::update_grains(float** buffer,
                                       size_t n_buffer_frames,
                                       uint32_t modulation_offset) {
    const auto& audio_data = m_audio_store.get_buffer();
    float density = get_parameter<float>(Density, modulation_offset);

    // Calculate how frequently we should trigger new grains
    unsigned int min_interval =
        static_cast<unsigned int>(m_sample_rate * k_min_grain_interval);  // max is 50 grains per
                                                                        // second
    unsigned int max_interval =
        static_cast<unsigned int>(m_sample_rate * k_max_grain_interval);  // min is 5 grains per
                                                                        // second
    unsigned int interval_range = max_interval - min_interval;
    m_min_grain_interval = max_interval - static_cast<unsigned int>(density * interval_range);

    size_t sample_index =
        static_cast<size_t>(std::clamp(get_parameter<int>(SampleIndex, modulation_offset),
                                       0,
                                       static_cast<int>(audio_data.size()) - 1));

    if (m_current_sample_index != sample_index) {
        m_current_sample_index = sample_index;
        m_next_grain_time = 0;
    }

    // Determine source channel count for the current sample
    size_t source_channels = 1;
    if (sample_index < audio_data.size() && !audio_data[sample_index].empty()) {
        source_channels = audio_data[sample_index].get_num_channels();
    }

    auto mode =
        static_cast<ChannelMode>(std::clamp(get_parameter<int>(ChannelModeParam, modulation_offset),
                                            0,
                                            static_cast<int>(ChannelMode::NumChannelModes) - 1));
    float spread = get_parameter<float>(Spread, modulation_offset);

    // Get total frames for visualization position normalization
    size_t total_frames = 0;
    if (sample_index < audio_data.size() && !audio_data[sample_index].empty()) {
        total_frames = audio_data[sample_index].get_num_frames();
    }

    // For each sample in the buffer
    for (unsigned int i = 0; i < n_buffer_frames; i++) {
        // Check if it's time to trigger a new grain
        if (m_next_grain_time <= 0) {
            trigger_grain(sample_index, modulation_offset);
            m_next_grain_time = m_min_grain_interval - 1;
        } else {
            m_next_grain_time--;
        }

        // Accumulate per-channel samples
        float channel_accum[k_max_channel_support] = {};

        // Process all active grains
        for (size_t gi = 0; gi < m_grains.size(); ++gi) {
            auto& grain = m_grains[gi];
            if (!grain.m_active) { continue; }

            // Hann window envelope for amplitude control
            float normalized_position =
                static_cast<float>(grain.m_current_position) / static_cast<float>(grain.m_grain_size);
            float envelope = grain.m_envelope.process_at_position(normalized_position);

            // Check if the grain should be deactivated
            if (!grain.m_envelope.is_active() || normalized_position >= 1.0f) {
                grain.m_active = false;
                for (auto* l : m_viz_listeners) { l->on_grain_finished(static_cast<int>(gi)); }
                continue;
            }

            // Calculate the current position in the source audio
            float source_pos = grain.m_start_position + (grain.m_current_position * grain.m_velocity);

            float position_spread = 0.5f + (grain.m_position_spread - 0.5f) * spread;

            switch (mode) {
                case ChannelMode::MonoToStereo: {
                    // Mono sum of all source channels
                    float mono_sample = 0.0f;
                    for (size_t ch = 0; ch < source_channels; ++ch) {
                        float s = 0.0f;
                        read_sample(source_pos, grain.m_sample_index, ch, s);
                        mono_sample += s;
                    }
                    if (source_channels > 1) { mono_sample /= static_cast<float>(source_channels); }
                    mono_sample *= envelope;
                    channel_accum[0] += mono_sample * (1.0f - position_spread);
                    channel_accum[1] += mono_sample * position_spread;
                    break;
                }
                case ChannelMode::TrueStereo: {
                    float s0 = 0.0f, s1 = 0.0f;
                    read_sample(source_pos, grain.m_sample_index, 0, s0);
                    if (source_channels > 1) {
                        read_sample(source_pos, grain.m_sample_index, 1, s1);
                    } else {
                        s1 = s0;
                    }
                    // Spread redistributes per-grain energy across L/R
                    channel_accum[0] += s0 * envelope * (1.0f - position_spread) * 2.0f;
                    channel_accum[1] += s1 * envelope * position_spread * 2.0f;
                    break;
                }
                case ChannelMode::TrueMultichannel: {
                    size_t out_channels = std::min(m_channels, source_channels);
                    for (size_t ch = 0; ch < out_channels; ++ch) {
                        float s = 0.0f;
                        read_sample(source_pos, grain.m_sample_index, ch, s);
                        // Even channels (0,2,...) get left energy, odd channels
                        // (1,3,...) get right energy
                        float energy = (ch % 2 == 0) ? (1.0f - position_spread) * 2.0f
                                                     : position_spread * 2.0f;
                        channel_accum[ch] += s * envelope * energy;
                    }
                    break;
                }
                default: break;
            }

            // Update grain position
            grain.m_current_position++;

            // Deactivate if grain is finished
            if (grain.m_current_position >= grain.m_grain_size) {
                grain.m_active = false;
                for (auto* l : m_viz_listeners) { l->on_grain_finished(static_cast<int>(gi)); }
            }
        }

        // Write to output buffer (planar layout)
        for (size_t ch = 0; ch < m_channels; ++ch) { buffer[ch][i] = channel_accum[ch]; }

        m_playback_elapsed_samples++;
    }

    // Rate-limited visualization update
    if (!m_viz_listeners.empty() && m_viz_update_interval > 0 && total_frames > 0) {
        m_viz_update_counter += n_buffer_frames;
        if (m_viz_update_counter >= m_viz_update_interval) {
            m_viz_update_counter = 0;
            float total_f = static_cast<float>(total_frames);

            // Send current master envelope value
            float master_env = m_envelope.get_current_level();
            for (auto* l : m_viz_listeners) { l->on_master_envelope_updated(master_env); }

            for (size_t gi = 0; gi < m_grains.size(); ++gi) {
                auto& grain = m_grains[gi];
                if (!grain.m_active) { continue; }
                float current_pos = static_cast<float>(grain.m_start_position +
                                                       grain.m_current_position * grain.m_velocity) /
                                    total_f;
                float normalized_position = static_cast<float>(grain.m_current_position) /
                                            static_cast<float>(grain.m_grain_size);
                float envelope = grain.m_envelope.process_at_position(normalized_position);
                for (auto* l : m_viz_listeners) {
                    l->on_grain_updated(static_cast<int>(gi), current_pos, envelope);
                }
            }
        }
    }
}

void GrainProcessorImpl::trigger_grain(const size_t sample_index, uint32_t modulation_offset) {
    const auto& audio_data = m_audio_store.get_buffer();

    // Find an inactive grain slot
    for (size_t gi = 0; gi < m_grains.size(); ++gi) {
        auto& grain = m_grains[gi];
        if (!grain.m_active) {
            if (sample_index >= audio_data.size() || audio_data[sample_index].empty()) { return; }

            // Get parameters needed for grain_size setup
            float grain_size_param = get_parameter<float>(Size, modulation_offset);
            float size_temperature = get_parameter<float>(TemperatureSize, modulation_offset);

            size_t grain_size = calculate_grain_size(grain_size_param, size_temperature);

            float velocity = get_parameter<float>(Velocity, modulation_offset);
            float velocity_temperature =
                get_parameter<float>(TemperatureVelocity, modulation_offset);
            velocity = calculate_velocity(velocity, velocity_temperature);

            // Apply sample start/end/loop region
            size_t total_frames = audio_data[sample_index].get_num_frames();
            auto region = compute_sample_region(total_frames, modulation_offset);
            if (region.size() == 0) { return; }

            auto effective_grain_size = static_cast<size_t>(std::ceil(grain_size * velocity));

            float position_temperature = apply_temperature_ramp(
                get_parameter<float>(TemperaturePosition, modulation_offset));
            long start_position = calculate_start_position(region, position_temperature);

            // Truncate grain if it would overshoot past region end
            long end_frame = static_cast<long>(region.m_end);
            long grain_end = start_position + static_cast<long>(effective_grain_size);
            if (grain_end > end_frame) {
                effective_grain_size =
                    static_cast<size_t>(std::max(0L, end_frame - start_position));
                if (effective_grain_size == 0) { return; }
                grain_size =
                    std::max(size_t(1), static_cast<size_t>(effective_grain_size / velocity));
            }

            // Setup the grain
            grain.m_start_position = start_position;
            grain.m_current_position = 0;
            grain.m_grain_size = grain_size;
            grain.m_velocity = velocity;
            grain.m_active = true;
            grain.m_sample_index = sample_index;
            grain.m_position_spread = m_uni_dist(m_random_generator);

            // Get the grain duration in milliseconds
            float grain_duration_ms = (grain_size / static_cast<float>(m_sample_rate)) * 1000.0f;

            // Configure the Hann window envelope
            grain.m_envelope.set_sample_rate(static_cast<float>(m_sample_rate));
            grain.m_envelope.set_duration(grain_duration_ms);
            grain.m_envelope.start();

            // Notify visualization listeners
            for (auto* l : m_viz_listeners) {
                float total = static_cast<float>(total_frames);
                l->on_grain_triggered(static_cast<int>(gi),
                                      static_cast<float>(start_position) / total,
                                      static_cast<float>(effective_grain_size) / total,
                                      velocity,
                                      grain_duration_ms);
            }

            return;
        }
    }
}

size_t GrainProcessorImpl::calculate_grain_size(float grain_size_param, float temperature) {
    auto min_size = static_cast<size_t>(k_min_grain_size * m_sample_rate);
    auto max_size = static_cast<size_t>(k_max_grain_size * m_sample_rate);
    size_t range = max_size - min_size;
    size_t grain_size = min_size + static_cast<size_t>(grain_size_param * range);

    // Randomize grain size based on temperature
    float rand_value = m_uni_dist(m_random_generator);  // [0, 1)
    rand_value = (rand_value * 2.f - 1.f) / 2.f;        // [-0.5, 0.5)
    rand_value *= std::pow(temperature, 3.f);
    auto lower_interval = static_cast<float>(grain_size - min_size);
    auto upper_interval = static_cast<float>(max_size - grain_size);
    if (rand_value < 0.f) {
        grain_size = static_cast<size_t>(grain_size + lower_interval * rand_value);
    } else {
        // Do not make the grain size much larger for small values
        grain_size = static_cast<size_t>(grain_size + upper_interval * rand_value * 0.3f +
                                         grain_size * rand_value);
    }
    return std::clamp(grain_size, min_size, max_size);
}

long GrainProcessorImpl::calculate_start_position(const SampleRegion& region, float temperature) {
    long start_position = static_cast<long>(m_sequential_position);

    // Apply temperature-based randomization to grain start position
    long max_position = static_cast<long>(region.size());
    if (max_position <= 0) { return static_cast<long>(region.m_start); }
    if (temperature == 0.f && start_position >= max_position) {
        return static_cast<long>(region.m_loop_point);
    }

    float rand_value = m_uni_dist(m_random_generator);  // [0, 1)
    rand_value = (rand_value - 0.5f) * 2.f;             // [-1, 1)
    rand_value *= temperature;
    start_position += static_cast<long>(rand_value * max_position);
    if (start_position >= max_position) {
        start_position -= max_position;
    } else if (start_position < 0) {
        start_position += max_position;
    }

    start_position += region.m_start;

    // Advance sequential position and handle looping.
    // Scanner traverses the full region (start → end), then restarts at loop_point.
    m_sequential_position += static_cast<long>(m_min_grain_interval);

    if (m_sequential_position >= max_position) {
        m_sequential_position = static_cast<long>(region.m_loop_point - region.m_start);
    }

    return start_position;
}

float GrainProcessorImpl::calculate_velocity(float velocity, float temperature) {
    float velocity_factor = m_uni_dist(m_random_generator);  // [0, 1)
    velocity_factor = (velocity_factor * 2.f - 1.f);         // [-1, 1)
    velocity_factor *= std::pow(temperature, 15.f);
    float semitone_factor = std::pow(2.f, 7.f / 12.f);
    if (velocity_factor < 0.f) {
        velocity /= (1.f - velocity_factor * (semitone_factor - 1.f));
    } else {
        velocity *= (1.f + velocity_factor * (semitone_factor - 1.f));
    }
    return velocity;
}

float GrainProcessorImpl::apply_temperature_ramp(float temperature) const {
    float ramp_samples = static_cast<float>(k_temperature_ramp_duration * m_sample_rate);
    float ramp_factor = 1.0f;
    if (static_cast<float>(m_playback_elapsed_samples) < ramp_samples) {
        ramp_factor = std::sin((static_cast<float>(m_playback_elapsed_samples) / ramp_samples) *
                               static_cast<float>(M_PI / 2.0));
    }
    // Blend between ramped and unramped: low temperature → ramp active, high →
    // ramp bypassed
    float ramped = temperature * ramp_factor;
    return ramped + (temperature - ramped) * temperature;
}

GrainProcessorImpl::SampleRegion GrainProcessorImpl::compute_sample_region(
    size_t total_frames,
    uint32_t modulation_offset) {
    auto start =
        static_cast<size_t>(get_parameter_float(SampleStart, modulation_offset) * total_frames);
    auto end =
        static_cast<size_t>(get_parameter_float(SampleEnd, modulation_offset) * total_frames);
    auto loop =
        static_cast<size_t>(get_parameter_float(SampleLoopPoint, modulation_offset) * total_frames);
    start = std::clamp(start, size_t(0), total_frames);
    end = std::clamp(end, start, total_frames);
    loop = std::clamp(loop, start, end);
    return {start, end, loop};
}

void GrainProcessorImpl::read_sample(float position,
                                     size_t sample_index,
                                     size_t source_channel,
                                     float& out_sample) {
    out_sample = 0.0f;

    const auto& audio_data = m_audio_store.get_buffer();

    // Bounds checking
    if (sample_index >= audio_data.size() || audio_data[sample_index].empty()) { return; }

    const auto& buf = audio_data[sample_index];
    if (source_channel >= buf.get_num_channels()) { return; }

    size_t num_frames = buf.get_num_frames();

    // Linear interpolation between samples for fractional positions
    long pos_floor = static_cast<long>(position);
    long pos_ceil = pos_floor + 1;
    float frac = position - static_cast<float>(pos_floor);

    // Ensure we don't read beyond the buffer
    while (pos_ceil >= static_cast<long>(num_frames)) { pos_ceil -= static_cast<long>(num_frames); }

    while (pos_floor >= static_cast<long>(num_frames)) {
        pos_floor -= static_cast<long>(num_frames);
    }

    const float* ch_data = buf.get_read_pointer(source_channel);
    out_sample = ch_data[pos_floor] * (1.0f - frac) + ch_data[pos_ceil] * frac;
}

}  // namespace thl::dsp::granular
