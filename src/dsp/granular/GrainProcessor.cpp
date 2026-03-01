#include <tanh/dsp/granular/GrainProcessor.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>

namespace thl::dsp::granular {

GrainProcessorImpl::GrainProcessorImpl(audio::AudioDataStore& audio_store)
    : m_audio_store(audio_store),
      m_max_grains(32),
      m_next_grain_time(0),
      m_min_grain_interval(100),
      m_sequential_position(0),
      m_random_generator(std::random_device{}()),
      m_uni_dist(0.0f, 1.0f),
      m_last_playing_state(false),
      m_current_note(0) {

    // Prepare grain container
    m_grains.resize(m_max_grains);

    // Initialize all grains as inactive
    for (auto& grain : m_grains) {
        grain.active = false;
    }
}

GrainProcessorImpl::~GrainProcessorImpl() = default;

void GrainProcessorImpl::reset_grains() {
    for (auto& grain : m_grains) {
        grain.active = false;
    }
    m_sequential_position = 0;
    m_next_grain_time = 0;
    m_last_playing_state = false;
    m_envelope.reset();
}

void GrainProcessorImpl::prepare(const double& sample_rate, const size_t& samples_per_block, const size_t& num_channels) {
    m_sample_rate = sample_rate;
    m_channels = num_channels;

    for (auto& grain : m_grains) {
        grain.envelope.set_sample_rate(static_cast<float>(m_sample_rate));
    }
    m_next_grain_time = 0;

    m_envelope.set_sample_rate(static_cast<float>(m_sample_rate));
    m_envelope.set_parameters(
        get_parameter<float>(EnvelopeAttack),
        get_parameter<float>(EnvelopeDecay),
        get_parameter<float>(EnvelopeSustain),
        get_parameter<float>(EnvelopeRelease)
    );
    m_envelope.reset();
}

void GrainProcessorImpl::process(float** buffer, const size_t& num_samples, const size_t& num_channels) {
    update_envelope_if_needed();

    bool playing = get_parameter<bool>(Playing);

    bool envelope_active = m_envelope.is_active();
    if (playing && !envelope_active || playing && !m_last_playing_state) {
        m_envelope.note_on();
        m_next_grain_time = 0; // Reset grain time when starting playback
        m_sequential_position = 0; // Reset sequential position when starting playback
    } else if (!playing && m_envelope.get_state() != utils::ADSR::State::IDLE && m_envelope.get_state() != utils::ADSR::State::RELEASE) {
        m_envelope.note_off();
    }
    m_last_playing_state = playing;

    float volume = get_parameter<float>(Volume);

    // Clear the buffer
    for (size_t ch = 0; ch < num_channels; ++ch) {
        std::memset(buffer[ch], 0, num_samples * sizeof(float));
    }

    // If not playing or no audio data, just return (silence)
    if (!m_envelope.is_active() || !m_audio_store.is_loaded()) {
        return;
    }

    // Process existing grains and generate new ones
    update_grains(buffer, num_samples);

    // Apply master volume
    for (size_t i = 0; i < num_samples; i++) {
        float grain_volume = volume * m_envelope.process();
        for (size_t ch = 0; ch < num_channels; ++ch) {
            buffer[ch][i] *= grain_volume;
        }
    }
}

void GrainProcessorImpl::update_envelope_if_needed() {
    float attack = get_parameter<float>(EnvelopeAttack);
    float decay = get_parameter<float>(EnvelopeDecay);
    float sustain = get_parameter<float>(EnvelopeSustain);
    float release = get_parameter<float>(EnvelopeRelease);

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
}

void GrainProcessorImpl::update_grains(float** buffer, size_t n_buffer_frames) {
    const auto& audio_data = m_audio_store.get_buffer();
    float density = get_parameter<float>(Density);

    // Calculate how frequently we should trigger new grains
    unsigned int min_interval = static_cast<unsigned int>(m_sample_rate * 0.02f); // max is 50 grains per second
    unsigned int max_interval = static_cast<unsigned int>(m_sample_rate * 0.2f);  // min is 5 grains per second
    unsigned int interval_range = max_interval - min_interval;
    m_min_grain_interval = max_interval - static_cast<unsigned int>(density * interval_range);

    size_t sample_index = static_cast<size_t>(std::clamp(get_parameter<int>(SampleIndex), 0, static_cast<int>(audio_data.size()) - 1));

    if (m_current_note != sample_index) {
        m_current_note = sample_index;
        m_next_grain_time = 0;
    }

    auto mode = static_cast<ChannelMode>(std::clamp(get_parameter<int>(ChannelModeParam), 0, 2));
    float spread = get_parameter<float>(Spread);

    // Determine source channel count for the current sample
    size_t source_channels = 1;
    if (sample_index < audio_data.size() && !audio_data[sample_index].empty()) {
        source_channels = audio_data[sample_index].get_num_channels();
    }

    // For each sample in the buffer
    for (unsigned int i = 0; i < n_buffer_frames; i++) {

        // Check if it's time to trigger a new grain
        if (m_next_grain_time <= 0) {
            trigger_grain(sample_index);
            m_next_grain_time = m_min_grain_interval - 1;
        } else {
            m_next_grain_time--;
        }

        // Accumulate per-channel samples
        float channel_accum[MAX_CHANNEL_SUPPORT] = {}; // Support up to 16 output channels

        // Process all active grains
        for (auto& grain : m_grains) {
            if (!grain.active) continue;

            // Calculate the normalized position in grain (0.0-1.0)
            float normalized_position = static_cast<float>(grain.current_position) / static_cast<float>(grain.grain_size);

            // Hann window envelope for amplitude control
            float envelope = grain.envelope.process_at_position(normalized_position);

            // Check if the grain should be deactivated
            if (!grain.envelope.is_active() || normalized_position >= 1.0f) {
                grain.active = false;
                continue;
            }

            // Calculate the current position in the source audio
            float source_pos = grain.start_position + (grain.current_position * grain.velocity);
            float env_gain = envelope * grain.gain;

            float position_spread = 0.5f + (grain.position_spread - 0.5f) * spread;

            switch (mode) {
                case ChannelMode::MonoToStereo: {
                    // Mono sum of all source channels
                    float mono_sample = 0.0f;
                    for (size_t ch = 0; ch < source_channels; ++ch) {
                        float s = 0.0f;
                        read_sample(source_pos, grain.sample_index, ch, s);
                        mono_sample += s;
                    }
                    if (source_channels > 1) {
                        mono_sample /= static_cast<float>(source_channels);
                    }
                    mono_sample *= env_gain;
                    channel_accum[0] += mono_sample * (1.0f - position_spread);
                    channel_accum[1] += mono_sample * position_spread;
                    break;
                }
                case ChannelMode::TrueStereo: {
                    float s0 = 0.0f, s1 = 0.0f;
                    read_sample(source_pos, grain.sample_index, 0, s0);
                    if (source_channels > 1) {
                        read_sample(source_pos, grain.sample_index, 1, s1);
                    } else {
                        s1 = s0;
                    }
                    // Spread redistributes per-grain energy across L/R
                    channel_accum[0] += s0 * env_gain * (1.0f - position_spread) * 2.0f;
                    channel_accum[1] += s1 * env_gain * position_spread * 2.0f;
                    break;
                }
                case ChannelMode::TrueMultichannel: {
                    size_t out_channels = std::min(m_channels, source_channels);
                    for (size_t ch = 0; ch < out_channels; ++ch) {
                        float s = 0.0f;
                        read_sample(source_pos, grain.sample_index, ch, s);
                        // Even channels (0,2,...) get left energy, odd channels (1,3,...) get right energy
                        float energy = (ch % 2 == 0) ? (1.0f - position_spread) * 2.0f : position_spread * 2.0f;
                        channel_accum[ch] += s * env_gain * energy;
                    }
                    break;
                }
            }

            // Update grain position
            grain.current_position++;

            // Deactivate if grain is finished
            if (grain.current_position >= grain.grain_size) {
                grain.active = false;
            }
        }

        // Write to output buffer (planar layout)
        for (size_t ch = 0; ch < m_channels; ++ch) {
            buffer[ch][i] = channel_accum[ch];
        }
    }
}

void GrainProcessorImpl::trigger_grain(const size_t note_number) {
    const auto& audio_data = m_audio_store.get_buffer();

    // Find an inactive grain slot
    for (auto& grain : m_grains) {
        if (!grain.active) {
            float temperature = get_parameter<float>(Temperature);
            float velocity = get_parameter<float>(Velocity);
            float grain_size_param = get_parameter<float>(Size);

            // Abort if this grain can not be played
            if (note_number >= audio_data.size() ||
                audio_data[note_number].empty()) {
                return;
            }

            // Calculate actual grain size in samples (0.02s to 0.4s)
            auto min_size = static_cast<size_t>(0.02 * m_sample_rate);
            auto max_size = static_cast<size_t>(0.4 * m_sample_rate);
            size_t range = max_size - min_size;
            size_t grain_size = min_size + static_cast<size_t>(grain_size_param * range);

            // randomize grain size
            float rand_value = m_uni_dist(m_random_generator); // Random value [0, 1)
            rand_value = (rand_value * 2.f - 1.f) / 2.f; // Scale to [-0.5, 0.5)
            rand_value *= std::pow(temperature, 3.f); // Scale by temperature
            auto lower_interval = static_cast<float>(grain_size - min_size);
            auto upper_interval = static_cast<float>(max_size - grain_size);
            if (rand_value < 0.f) {
                grain_size = static_cast<size_t>(grain_size + lower_interval * rand_value);
            } else {
                // do not make the grain size much larger for small values
                grain_size = static_cast<size_t>(grain_size + upper_interval * rand_value * 0.3f + grain_size * rand_value);
            }
            grain_size = std::clamp(grain_size, min_size, max_size); // Clamp to valid range

            long max_position = audio_data[note_number].get_num_frames() - grain_size;

            // Apply randomness based on temperature parameter
            long start_position = m_sequential_position;
            float grain_size_factor = static_cast<float>(grain_size) / static_cast<float>(max_size);
            float gain = 1.f;

            if (m_sequential_position != 0 || grain_size_factor < 0.6f) {
                rand_value = m_uni_dist(m_random_generator); // Random value [0, 1)
                rand_value = rand_value - 0.5f; // Scale to [-0.5, 0.5)
                rand_value *= temperature; // Scale by temperature
                start_position += static_cast<long>(rand_value * max_position);
                if (start_position >= max_position) {
                    start_position -= max_position; // Loop
                } else if (start_position < 0) {
                    start_position = start_position + max_position; // Loop
                }

                if (m_envelope.get_state() != utils::ADSR::State::ATTACK) {
                    // If the grain size is small and the start position is close to the start or end of the sample, then push it away from the edges
                    float start_position_factor = static_cast<float>(start_position) / static_cast<float>(max_position);
                    if (start_position_factor > 0.5f) {
                        start_position_factor -= 0.5f;
                        start_position_factor = start_position_factor * std::pow(1.f - grain_size_factor, 2.f);
                        start_position = start_position - static_cast<long>(start_position_factor * max_position);
                    } else {
                        start_position_factor = 0.5f - start_position_factor;
                        start_position_factor = start_position_factor * std::pow(1.f - grain_size_factor, 2.f);
                        start_position = start_position + static_cast<long>(start_position_factor * max_position);
                    }
                    gain += start_position_factor * 0.5f; // Increase gain if close to the edges
                }

                // check that max_position is positive
                if (max_position > 0) {
                    start_position = std::clamp(start_position, 0L, static_cast<long>(max_position));
                } else {
                    start_position = 0;
                }

            }

            m_sequential_position += grain_size / 2; // Overlap grains
            if (m_sequential_position >= max_position) {
                m_sequential_position -= static_cast<long>(0.6f * max_position); // Loop but do not replay the beginning
            }

            // Randomize the velocity
            float velocity_factor = m_uni_dist(m_random_generator); // Random value [0, 1)
            velocity_factor = (velocity_factor * 2.f - 1.f); // Scale to [-1, 1)
            velocity_factor *= std::pow(temperature, 15.f); // Scale by temperature
            float semitone_factor = std::pow(2.f, 7.f/12.f);
            // lower velocity is one semitone
            if (velocity_factor < 0.f) {
                velocity /= (1.f - velocity_factor * (semitone_factor - 1.f));
            } else {
                velocity *= (1.f + velocity_factor * (semitone_factor - 1.f));
            }

            // Get the grain duration in milliseconds
            float grain_duration_ms = (grain_size / static_cast<float>(m_sample_rate)) * 1000.0f;

            // Setup the grain
            grain.start_position = start_position;
            grain.current_position = 0;
            grain.grain_size = grain_size;
            grain.gain = gain;
            grain.velocity = velocity;
            grain.amplitude = 0.0f; // Start with zero amplitude for fade-in
            grain.active = true;
            grain.sample_index = note_number;
            grain.position_spread = m_uni_dist(m_random_generator); // Random position_spread [0, 1] for spread

            // Configure the Hann window envelope
            grain.envelope.set_sample_rate(static_cast<float>(m_sample_rate));
            grain.envelope.set_duration(grain_duration_ms);
            grain.envelope.start();

            return;
        }
    }
}

void GrainProcessorImpl::read_sample(float position, size_t sample_index, size_t source_channel, float& out_sample) {
    out_sample = 0.0f;

    const auto& audio_data = m_audio_store.get_buffer();

    // Bounds checking
    if (sample_index >= audio_data.size() || audio_data[sample_index].empty()) {
        return;
    }

    const auto& buf = audio_data[sample_index];
    if (source_channel >= buf.get_num_channels()) {
        return;
    }

    size_t num_frames = buf.get_num_frames();

    // Linear interpolation between samples for fractional positions
    long pos_floor = static_cast<long>(position);
    long pos_ceil = pos_floor + 1;
    float frac = position - static_cast<float>(pos_floor);

    // Ensure we don't read beyond the buffer
    while (pos_ceil >= static_cast<long>(num_frames)) {
        pos_ceil -= static_cast<long>(num_frames);
    }

    while (pos_floor >= static_cast<long>(num_frames)) {
        pos_floor -= static_cast<long>(num_frames);
    }

    const float* ch_data = buf.get_read_pointer(source_channel);
    out_sample = ch_data[pos_floor] * (1.0f - frac) + ch_data[pos_ceil] * frac;
}

} // namespace thl::dsp::granular
