#include <tanh/dsp/granular/GrainProcessor.h>
#include <cstring>
#include <algorithm>
#include <iostream>

namespace thl::dsp::granular {

GrainProcessorImpl::GrainProcessorImpl(size_t grain_index, audio::AudioDataStore& audio_store)
    : m_audio_store(audio_store),
      m_grain_index(grain_index),
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

    m_internal_buffer.clear();
    m_internal_buffer.resize(samples_per_block * 2); // stereo buffer
}

void GrainProcessorImpl::process(float** buffer, const size_t& num_samples, const size_t& num_channels) {
    this->process(m_internal_buffer.data(), static_cast<unsigned int>(num_samples));

    process_voice_fx(m_internal_buffer.data(), num_samples, num_channels, m_grain_index, m_envelope.is_active());

    // Mix into main output buffer
    if (num_channels >= 1) {
        float* left_out = buffer[0];
        const float* left_in = m_internal_buffer.data();
        for (size_t i = 0; i < num_samples; ++i) {
            left_out[i] += left_in[i];
        }
    }

    if (num_channels >= 2) {
        float* right_out = buffer[1];
        const float* right_in = m_internal_buffer.data() + num_samples;
        for (size_t i = 0; i < num_samples; ++i) {
            right_out[i] += right_in[i];
        }
    }
}

void GrainProcessorImpl::process(float* output_buffer, unsigned int n_buffer_frames) {
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
    std::memset(output_buffer, 0, n_buffer_frames * 2 * sizeof(float));

    // If not playing or no audio data, just return (silence)
    if (!m_envelope.is_active() || !m_audio_store.is_loaded()) {
        return;
    }

    // Process existing grains and generate new ones
    update_grains(output_buffer, n_buffer_frames);

    // Apply master volume
    for (unsigned int i = 0; i < n_buffer_frames; i++) {
        float grain_volume = volume * m_envelope.process();
        output_buffer[i] *= grain_volume;
        output_buffer[i + n_buffer_frames] *= grain_volume;
    }
}

void GrainProcessorImpl::trigger_grain(const size_t note_number) {
    // Find an inactive grain slot
    for (auto& grain : m_grains) {
        if (!grain.active) {
            float temperature = get_parameter<float>(Temperature);
            float velocity = get_parameter<float>(Velocity);
            float grain_size_param = get_parameter<float>(Size);

            // Map the MIDI note number to a sample index
            int local_sample_index = map_midi_note_to_sample_index(static_cast<int>(note_number));

            const auto& audio_data = m_audio_store.get_active();

            // Abort if this grain can not be played
            if (local_sample_index < 0 ||
                static_cast<size_t>(local_sample_index) >= audio_data.size() ||
                audio_data[local_sample_index].empty()) {
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

            long max_position = audio_data[local_sample_index].size() / m_channels - grain_size;

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
            grain.sample_index = local_sample_index;

            // Configure the Hann window envelope
            grain.envelope.set_sample_rate(static_cast<float>(m_sample_rate));
            grain.envelope.set_duration(grain_duration_ms);
            grain.envelope.start();

            return;
        }
    }
}

void GrainProcessorImpl::update_grains(float* output_buffer, unsigned int n_buffer_frames) {
    float density = get_parameter<float>(Density);
    auto mode = static_cast<utils::ScaleMode>(get_parameter<int>(KeyMode));
    int root_note = get_parameter<int>(RootNote);

    // Calculate how frequently we should trigger new grains
    unsigned int min_interval = static_cast<unsigned int>(m_sample_rate * 0.02f); // max is 50 grains per second
    unsigned int max_interval = static_cast<unsigned int>(m_sample_rate * 0.2f);  // min is 5 grains per second
    unsigned int interval_range = max_interval - min_interval;
    m_min_grain_interval = max_interval - static_cast<unsigned int>(density * interval_range);

    // Get the sample index parameter and map it to a note in the selected scale
    float sample_index = get_parameter<float>(SampleIndex);
    sample_index = (sample_index * 15.3f) - 0.3f;

    size_t sample_index_quantized = static_cast<size_t>(sample_index);
    sample_index = root_note + utils::get_note_offset(sample_index_quantized, mode);

    if (m_current_note != static_cast<size_t>(sample_index)) {
        m_current_note = static_cast<size_t>(sample_index);
        m_next_grain_time = 0;
    }

    const auto& audio_data = m_audio_store.get_active();

    // For each sample in the buffer
    for (unsigned int i = 0; i < n_buffer_frames; i++) {

        // Check if it's time to trigger a new grain
        if (m_next_grain_time <= 0) {
            trigger_grain(static_cast<size_t>(sample_index));
            m_next_grain_time = m_min_grain_interval - 1;
        } else {
            m_next_grain_time--;
        }

        float left_sample = 0.0f;
        float right_sample = 0.0f;

        // Process all active grains
        for (auto& grain : m_grains) {
            if (grain.active) {

                float samples[2] = {0.0f, 0.0f}; // Temporary buffer for sample values

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
                // Get the interpolated sample value
                get_sample_with_interpolation(source_pos, samples, grain.sample_index);

                // Apply envelope
                samples[0] *= envelope * grain.gain;
                samples[1] *= envelope * grain.gain;

                // Mix into output buffer
                left_sample += samples[0];
                right_sample += samples[1];

                // Update grain position
                grain.current_position++;

                // Deactivate if grain is finished
                if (grain.current_position >= grain.grain_size) {
                    grain.active = false;
                }
            }
        }

        // Write to output buffer
        output_buffer[i] = left_sample;
        output_buffer[i + n_buffer_frames] = right_sample;
    }
}

void GrainProcessorImpl::get_sample_with_interpolation(float position, float* samples, size_t sample_index) {
    samples[0] = 0.0f;
    samples[1] = 0.0f;

    const auto& audio_data = m_audio_store.get_active();

    // Bounds checking
    if (sample_index >= audio_data.size() || audio_data[sample_index].empty()) {
        return;
    }

    // Linear interpolation between samples for fractional positions
    long pos_floor = static_cast<long>(position);
    long pos_ceil = pos_floor + 1;
    float frac = position - pos_floor;

    size_t buflimit = audio_data[sample_index].size() / m_channels;
    // Ensure we don't read beyond the buffer
    while (pos_ceil >= buflimit) {
        pos_ceil -= buflimit;
    }

    while (pos_floor >= buflimit) {
        pos_floor -= buflimit;
    }

    // Linear interpolation for the sample value
    if (m_channels == 1) {
        float sample = audio_data[sample_index][pos_floor] * (1.0f - frac) + audio_data[sample_index][pos_ceil] * frac;
        samples[0] = sample;
        samples[1] = sample;
    } else {
        // For stereo, get the average of left and right channels
        samples[0] = audio_data[sample_index][pos_floor] * (1.0f - frac) + audio_data[sample_index][pos_ceil] * frac;
        samples[1] = audio_data[sample_index][pos_floor + buflimit] * (1.0f - frac) + audio_data[sample_index][pos_ceil + buflimit] * frac;
    }
}

} // namespace thl::dsp::granular
