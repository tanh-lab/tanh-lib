#include <tanh/dsp/granular/GrainProcessor.h>

#include <choc/audio/choc_AudioFileFormat.h>
#include <choc/audio/choc_AudioFileFormat_WAV.h>
#include <choc/audio/choc_AudioFileFormat_MP3.h>

#include <iostream>

struct Samplepack
{
    int note_lowest;
    int note_highest;
    std::string path_to_sample_folder;
    float gain = 1.0f;

    int note_number() const {
        return this->note_highest - this->note_lowest + 1;
    }

    std::string get_sample_path(int note_number) {
        std::string note_name = thl::dsp::utils::note_number_to_note_name(note_number);
        int octave = (note_number / 12) - 1; // Calculate the octave

        std::string file_path = this->path_to_sample_folder +
                               std::to_string(note_number) + "_" + note_name +
                               std::to_string(octave) + "_00_00.mp3";
        return file_path;
    }
};

const float global_gain = 0.7;
const std::vector<Samplepack> samplepacks = {
    {60, 94, "path/to/assets/samplepacks_mp3/HurdyGurdy/", 1.266f * global_gain},      // added samples properly start with C at 60, samples start at 67 (that's a G), but at least they're tuned correctly
    {52, 86, "path/to/assets/samplepacks_mp3/Mellotron/", 0.878f * global_gain},       // samples start at 48; C at 52
    {48, 102, "path/to/assets/samplepacks_mp3/Clavichord/", 21.f * global_gain},  // samples start at 40; C at 48
    {47, 84, "path/to/assets/samplepacks_mp3/Regal-Organ/", 0.9f * global_gain},      // samples start at 36; C at 47
    {48, 88, "path/to/assets/samplepacks_mp3/Glasharmonica/", 0.7f * global_gain}      // samples start at 48 (if missing have been created); C at 48 (officially it starts at 50, with c at 60)
};

namespace thl::dsp::granular
{

GrainProcessorImpl::GrainProcessorImpl(size_t grain_index)
    : m_grain_index(grain_index), m_num_notes(35), m_max_grains(32),
      m_next_grain_time(0), m_min_grain_interval(100), m_sequential_position(0),
      m_random_generator(std::random_device{}()), m_uni_dist(0.0f, 1.0f), m_last_playing_state(false) {

    // Prepare grain container
    m_grains.resize(m_max_grains);

    // Initialize all grains as inactive
    for (auto& grain : m_grains) {
        grain.active = false;
    }

}

GrainProcessorImpl::~GrainProcessorImpl() {
    // No need for special cleanup with std::vector
}

void GrainProcessorImpl::init()
{
    if(m_audio_data.empty()){
        if (!prepare_audio_data()) {
            std::cerr << "Failed to prepare audio data" << std::endl;
        }
    }
}

void GrainProcessorImpl::prepare(const double& sample_rate, const size_t& samples_per_block, const size_t& num_channels)
{
    m_sample_rate = sample_rate;
    m_channels = num_channels;

    for (auto& grain : m_grains) {
        grain.envelope.set_sample_rate(static_cast<float>(m_sample_rate));
    }
    m_next_grain_time = 0;

    m_envelope.set_sample_rate(static_cast<float>(m_sample_rate));
    m_envelope.set_parameters(
        getParameterFloat(GlobalEnvelopeAttack),
        getParameterFloat(GlobalEnvelopeDecay),
        getParameterFloat(GlobalEnvelopeSustain),
        getParameterFloat(GlobalEnvelopeRelease)
        // m_state.get<float>("screen_" + std::to_string(m_screen_index) + ".grains.grain_" + std::to_string(m_grain_index) + ".envelope.attack"), // Convert ms to seconds
        // m_state.get<float>("screen_" + std::to_string(m_screen_index) + ".grains.grain_" + std::to_string(m_grain_index) + ".envelope.decay"),  // Convert ms to seconds
        // m_state.get<float>("screen_" + std::to_string(m_screen_index) + ".grains.grain_" + std::to_string(m_grain_index) + ".envelope.sustain"),
        // m_state.get<float>("screen_" + std::to_string(m_screen_index) + ".grains.grain_" + std::to_string(m_grain_index) + ".envelope.release") // Convert ms to seconds
    );
    m_envelope.reset();

    m_internal_buffer.clear();
    m_internal_buffer.resize(samples_per_block * 2); // stereo buffer
}

void GrainProcessorImpl::process(float** buffer, const size_t& num_samples, const size_t& num_channels)
{
    this->process(m_internal_buffer.data(), static_cast<unsigned int>(num_samples));

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
    // // Allocate a temporary buffer on the stack for processing
    // char buffer[256];
    // // Get current state parameters
    // snprintf(buffer, sizeof(buffer), "screen_%zu.grains.grain_%zu.play", m_screen_index, m_grain_index);
    // bool playing = m_state.get<bool>(buffer);
    bool playing = getParameterBool(Playing);

    bool envelope_active = m_envelope.is_active();
    if (playing && !envelope_active || playing && !m_last_playing_state) {
        m_envelope.note_on();
        m_next_grain_time = 0; // Reset grain time when starting playback
        m_sequential_position = 0; // Reset sequential position when starting playback
    } else if (!playing && m_envelope.get_state() != utils::ADSR::State::IDLE && m_envelope.get_state() != utils::ADSR::State::RELEASE) {
        m_envelope.note_off();
    }
    m_last_playing_state = playing;

    // snprintf(buffer, sizeof(buffer), "screen_%zu.grains.grain_%zu.volume", m_screen_index, m_grain_index);
    // float volume = m_state.get<float>(buffer);
    float volume = getParameterFloat(Volume);

    // Clear the buffer - needed if not playing
    std::memset(output_buffer, 0, n_buffer_frames * 2 * sizeof(float)); // 2 channels

    // If not playing or no audio data, just return (silence)
    if (!m_envelope.is_active() || m_audio_data.empty()) {
        return;
    }

    // Process existing grains and generate new ones
    update_grains(output_buffer, n_buffer_frames);

    // Apply master volume
    for (unsigned int i = 0; i < n_buffer_frames; i++) {
        float grain_volume = volume * m_envelope.process();
        output_buffer[i] *= grain_volume;
        output_buffer[i + n_buffer_frames] *= grain_volume; // right channel
    }
}

bool GrainProcessorImpl::prepare_audio_data() {

    if (!load_all_samples()){
        return false;
    }

    return true;
}

bool GrainProcessorImpl::load_all_samples(){

    m_audio_data.clear();
    m_audio_data.resize(samplepacks.size());
    for (size_t sample_pack_index = 0; sample_pack_index < samplepacks.size(); sample_pack_index++){

        auto sample_pack = samplepacks[sample_pack_index];
        int n_samples = std::min(sample_pack.note_number(), static_cast<int>(m_num_notes));
        if (n_samples < m_num_notes){
            std::cout << "Warning: sample pack at " << sample_pack.path_to_sample_folder << " has " << n_samples << " instead of required " << m_num_notes << std::endl;
        }
        // TODO handle proper resizing
        m_audio_data[sample_pack_index].resize(m_num_notes);

        for (size_t sample_index = 0; sample_index < n_samples; ++sample_index) {
            int note_number = sample_pack.note_lowest + sample_index; // Starting from 51_D#3
            std::string file_path = sample_pack.get_sample_path(note_number);
            if (!load_mp3_file(file_path, sample_pack_index, sample_index, sample_pack.gain)) {
                std::cerr << "Failed to load audio file for grain " << sample_index << ": " <<  file_path << std::endl;
                return false;
            }
        }
    }
    return true;
}

bool GrainProcessorImpl::load_wav_file(const std::string& file_path, const size_t sample_pack_index, const size_t sample_index, const float gain) {
    // Clear current data
    m_audio_data[sample_pack_index][sample_index].clear();

    // Create a reader using choc
    choc::audio::AudioFileFormatList formatList;
    formatList.addFormat<choc::audio::WAVAudioFileFormat<false>>();

    auto reader = formatList.createReader(file_path);
    if (!reader) {
        std::cerr << "Error opening audio file: " << file_path << std::endl;
        return false;
    }

    auto props = reader->getProperties();

    // Store file information
    if (m_channels == -1 || m_sample_rate == -1){
        m_channels = props.numChannels;
        m_sample_rate = props.sampleRate;
    } else if (
        m_channels != props.numChannels || m_sample_rate != props.sampleRate
    ) {
        std::cerr << "Error opening audio file: invalid channel count or samplerate" << std::endl;
        // Proceeding anyway as strictly matching might be too restrictive if we just want to read what we can?
        // Original code printed error but didn't return false, but it did exit(1) on sample rate mismatch.
    }
    
    if (m_sample_rate != 48000) {
         std::cerr << "Sample rate mismatch: expected "
                   << "48000.0"
                   << ", got " << m_sample_rate << std::endl;
         exit(1);
    }

    // Determine read range
    long start_frame = getParameterInt(SampleStart);
    long end_frame = getParameterInt(SampleEnd);

    // Validate range against file length
    if (end_frame > props.numFrames) end_frame = static_cast<long>(props.numFrames);
    if (start_frame >= end_frame) start_frame = 0; // or handle error

    long frames_to_read = end_frame - start_frame;

    // Allocate memory for audio data (planar: LLL...RRR...)
    m_audio_data[sample_pack_index][sample_index].resize(frames_to_read * m_channels);

    // Create a view into our vector that choc can write to
    // We need to construct an array of pointers to the channel starts
    std::vector<float*> channel_pointers(m_channels);
    for (size_t ch = 0; ch < m_channels; ++ch) {
        channel_pointers[ch] = m_audio_data[sample_pack_index][sample_index].data() + (ch * frames_to_read);
    }

    auto view = choc::buffer::createChannelArrayView(channel_pointers.data(), (unsigned int)m_channels, (unsigned int)frames_to_read);

    // Read the audio data directly into our planar buffer
    if (!reader->readFrames(start_frame, view)) {
         std::cerr << "Error reading audio data from " << file_path << std::endl;
         return false;
    }

    // Apply gain to all samples if necessary
    if (gain != 1.0f){
        for (float & i : m_audio_data[sample_pack_index][sample_index]) {
            i = i * gain;
        }
    }

    std::cout << "Loaded audio file: " << file_path << std::endl;
    std::cout << "Channels: " << m_channels << ", Sample rate: " << m_sample_rate
             << ", Frames: " << frames_to_read << "/" << props.numFrames << std::endl;

    return true;
}

bool GrainProcessorImpl::load_mp3_file(const std::string& file_path, const size_t sample_pack_index, const size_t sample_index, const float gain) {
    // Clear current data
    m_audio_data[sample_pack_index][sample_index].clear();

    // Create a reader using choc
    choc::audio::AudioFileFormatList formatList;
    formatList.addFormat<choc::audio::MP3AudioFileFormat>();

    auto reader = formatList.createReader(file_path);
    if (!reader) {
        std::cerr << "Error opening audio file: " << file_path << std::endl;
        return false;
    }

    auto props = reader->getProperties();

    // Store file information
    if (m_channels == -1 || m_sample_rate == -1){
        m_channels = props.numChannels;
        m_sample_rate = props.sampleRate;
    } else if (
        m_channels != props.numChannels || m_sample_rate != props.sampleRate
    ) {
        std::cerr << "Error opening audio file: invalid channel count or samplerate" << std::endl;
    }
    
    if (m_sample_rate != 48000) {
         std::cerr << "Sample rate mismatch: expected "
                   << "48000.0"
                   << ", got " << m_sample_rate << std::endl;
         exit(1);
    }

    // Determine read range
    long start_frame = getParameterInt(SampleStart);
    long end_frame = getParameterInt(SampleEnd);

    // Validate range against file length
    if (end_frame > props.numFrames) end_frame = static_cast<long>(props.numFrames);
    if (start_frame >= end_frame) start_frame = 0; // or handle error

    long frames_to_read = end_frame - start_frame;

    // Allocate memory for audio data (planar: LLL...RRR...)
    m_audio_data[sample_pack_index][sample_index].resize(frames_to_read * m_channels);

    // Create a view into our vector that choc can write to
    std::vector<float*> channel_pointers(m_channels);
    for (size_t ch = 0; ch < m_channels; ++ch) {
        channel_pointers[ch] = m_audio_data[sample_pack_index][sample_index].data() + (ch * frames_to_read);
    }

    auto view = choc::buffer::createChannelArrayView(channel_pointers.data(), (unsigned int)m_channels, (unsigned int)frames_to_read);

    // Read the audio data directly into our planar buffer
    if (!reader->readFrames(start_frame, view)) {
         std::cerr << "Error reading audio data from " << file_path << std::endl;
         return false;
    }

    // Apply gain to all samples if necessary
    if (gain != 1.0f){
        for (float & i : m_audio_data[sample_pack_index][sample_index]) {
            i = i * gain;
        }
    }

    std::cout << "Loaded audio file: " << file_path << std::endl;
    std::cout << "Channels: " << m_channels << ", Sample rate: " << m_sample_rate
             << ", Frames: " << frames_to_read << "/" << props.numFrames << std::endl;

    return true;
}

bool GrainProcessorImpl::load_mp3_from_memory(const char* data, int size, size_t sample_pack_index, size_t sample_index, float gain) {
    if (data == nullptr || size == 0) return false;

    // Clear current data
    if (m_audio_data.size() <= sample_pack_index) m_audio_data.resize(sample_pack_index + 1);
    if (m_audio_data[sample_pack_index].size() <= sample_index) m_audio_data[sample_pack_index].resize(sample_index + 1);
    m_audio_data[sample_pack_index][sample_index].clear();

    // Create a reader using choc from memory
    choc::audio::AudioFileFormatList formatList;
    formatList.addFormat<choc::audio::MP3AudioFileFormat>();

    auto stream = std::make_shared<std::istringstream>(std::string(data, static_cast<size_t>(size)), std::ios::binary);
    auto reader = formatList.createReader(std::move(stream));
    if (!reader) {
        std::cerr << "Error opening audio data from memory" << std::endl;
        return false;
    }

    auto props = reader->getProperties();

    // Store file information
    if (m_channels == -1 || m_sample_rate == -1){
        m_channels = props.numChannels;
        m_sample_rate = props.sampleRate;
    } 
    
    if (m_sample_rate != 48000) {
         // std::cerr << "Sample rate mismatch: expected 48000.0, got " << m_sample_rate << std::endl;
    }

    long frames_to_read = props.numFrames;

    // Allocate memory for audio data (planar: LLL...RRR...)
    m_audio_data[sample_pack_index][sample_index].resize(frames_to_read * m_channels);

    // Create a view into our vector that choc can write to
    std::vector<float*> channel_pointers(m_channels);
    for (size_t ch = 0; ch < m_channels; ++ch) {
        channel_pointers[ch] = m_audio_data[sample_pack_index][sample_index].data() + (ch * frames_to_read);
    }

    auto view = choc::buffer::createChannelArrayView(channel_pointers.data(), (unsigned int)m_channels, (unsigned int)frames_to_read);

    // Read the audio data directly into our planar buffer
    if (!reader->readFrames(0, view)) {
         std::cerr << "Error reading audio data from memory" << std::endl;
         return false;
    }

    // Apply gain to all samples if necessary
    if (gain != 1.0f){
        for (float & i : m_audio_data[sample_pack_index][sample_index]) {
            i = i * gain;
        }
    }

    return true;
}

void GrainProcessorImpl::trigger_grain(const size_t note_number) {
    // Find an inactive grain slot
    for (auto& grain : m_grains) {
        if (!grain.active) {
            // char buffer[256];
            // Initialize grain parameters
            // sprintf(buffer, "screen_%zu.grains.grain_%zu.temperature", m_screen_index, m_grain_index);
            // float temperature = m_state.get<float>(buffer);
            float temperature = getParameterFloat(Temperature);
            // sprintf(buffer, "screen_%zu.grains.grain_%zu.velocity", m_screen_index, m_grain_index);
            // float velocity = m_state.get<float>(buffer);
            float velocity = getParameterFloat(Velocity);
            // sprintf(buffer, "screen_%zu.grains.grain_%zu.size", m_screen_index, m_grain_index);
            // float grain_size_param = m_state.get<float>(buffer);
            float grain_size_param = getParameterFloat(Size);
            // sprintf(buffer, "screen_%zu.grains.grain_%zu.density", m_screen_index, m_grain_index);
            // float density = m_state.get<float>(buffer);
            float density = getParameterFloat(Density);

            // get index of the currently active sample pack, clamp it to legal boundaries
            // sprintf(buffer, "screen_%zu.grains.grain_%zu.sample_pack_index", m_screen_index, m_grain_index);
            // int sample_pack_index = m_state.get<int>(buffer);
            int sample_pack_index = getParameterInt(SamplePackIndex);


            sample_pack_index = std::max(std::min(sample_pack_index, static_cast<int>(samplepacks.size())-1), 0);

            // FIX: Map the global MIDI note number to a local index within the sample pack.
            // The audio data vector is 0-indexed (0 to num_notes-1), but the note number 
            // is an absolute MIDI pitch (e.g., 60 for C4).
            // We must subtract the sample pack's lowest note to get the correct 0-based index.
            // In the original codebase, this likely worked because RootNote default was 0.
            int local_sample_index = static_cast<int>(note_number) - samplepacks[sample_pack_index].note_lowest;

            // abort if this grain can not be played (index out of bounds or empty data)
            if (local_sample_index < 0 || 
                local_sample_index >= m_audio_data[sample_pack_index].size() || 
                m_audio_data[sample_pack_index][local_sample_index].empty()) {
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

            long max_position = m_audio_data[sample_pack_index][local_sample_index].size() / m_channels - grain_size;

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
                if (max_position > 0){
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
            float grain_duration_ms = (grain_size / (float)m_sample_rate) * 1000.0f;

            // Setup the grain
            grain.start_position = start_position;
            grain.current_position = 0;
            grain.grain_size = grain_size;
            grain.gain = gain;
            grain.velocity = velocity;
            grain.amplitude = 0.0f; // Start with zero amplitude for fade-in
            grain.active = true;
            grain.sample_index = local_sample_index;
            grain.sample_pack_index = sample_pack_index;
            // std::cout << "triggering grain with spi=" << sample_pack_index << " and si=" << local_sample_index << std::endl;
            // Configure the Hann window envelope
            grain.envelope.set_sample_rate(static_cast<float>(m_sample_rate));
            grain.envelope.set_duration(grain_duration_ms);
            grain.envelope.start();

            return;
        }
    }
}

void GrainProcessorImpl::update_grains(float* output_buffer, unsigned int n_buffer_frames) {
    char buffer[256];

    // snprintf(buffer, sizeof(buffer), "screen_%zu.grains.grain_%zu.density", m_screen_index, m_grain_index);
    //float density = m_state.get<float>(buffer);
    float density = getParameterFloat(Density);

    //ScaleMode mode = (ScaleMode) m_state.get<int>("player.key.mode");
    auto mode = static_cast<utils::ScaleMode>(getParameterInt(KeyMode));

    // int root_note = m_state.get<int>("player.key.root");
    int root_note = getParameterInt(RootNote);

    // Calculate how frequently we should trigger new grains
    unsigned int min_interval = static_cast<unsigned int>(m_sample_rate * 0.02f); // max is 50 grains per second
    unsigned int max_interval = static_cast<unsigned int>(m_sample_rate * 0.2f);  // min is 5 grains per second
    unsigned int interval_range = max_interval - min_interval;
    m_min_grain_interval = max_interval - static_cast<unsigned int>(density * interval_range);

    // Get the sample index parameter and map it to a note in the selected scale
    // snprintf(buffer, sizeof(buffer), "screen_%zu.grains.grain_%zu.sample_index", m_screen_index, m_grain_index);
    // float sample_index = m_state.get<float>(buffer);
    float sample_index = getParameterFloat(SampleIndex);
    sample_index = (sample_index * 15.3f) - 0.3f; // Scale to -0.3f - 14.99 range

    size_t sample_index_quantized = static_cast<size_t>(sample_index);

    sample_index = root_note +  utils::get_note_offset(sample_index_quantized, (utils::ScaleMode) mode);

    if (m_current_note != static_cast<size_t>(sample_index)) {
        m_current_note = static_cast<size_t>(sample_index);
        // Reset the grain time when changing notes
        m_next_grain_time = 0;
    }

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
                get_sample_with_interpolation(source_pos, samples, grain.sample_pack_index, grain.sample_index);

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

void GrainProcessorImpl::get_sample_with_interpolation(float position, float* samples, size_t sample_pack_index, size_t sample_index) {
    samples[0] = 0.0f;
    samples[1] = 0.0f;
    // XXX for some reason this does not work? it causes the audio to crack a lot
    // auto sample_file = m_audio_data[sample_pack_index][sample_index];

    // Bounds checking
    if (sample_index >= m_audio_data[sample_pack_index].size() || m_audio_data[sample_pack_index][sample_index].empty()) {
        return;
    }

    // Linear interpolation between samples for fractional positions
    long pos_floor = static_cast<long>(position);
    long pos_ceil = pos_floor + 1;
    float frac = position - pos_floor;

    size_t buflimit = m_audio_data[sample_pack_index][sample_index].size() / m_channels;
    // Ensure we don't read beyond the buffer
    while (pos_ceil >= buflimit) {
        pos_ceil -= buflimit;
    }

    while (pos_floor >= buflimit) {
        pos_floor -= buflimit;
    }

    // Linear interpolation for the sample value
    if (m_channels == 1) {
        float sample = m_audio_data[sample_pack_index][sample_index][pos_floor] * (1.0f - frac) + m_audio_data[sample_pack_index][sample_index][pos_ceil] * frac;
        samples[0] = sample; // left channel
        samples[1] = sample; // right channel
    } else {
        // For stereo, get the average of left and right channels
        samples[0] = m_audio_data[sample_pack_index][sample_index][pos_floor] * (1.0f - frac) + m_audio_data[sample_pack_index][sample_index][pos_ceil] * frac;
        samples[1] = m_audio_data[sample_pack_index][sample_index][pos_floor + buflimit] * (1.0f - frac) + m_audio_data[sample_pack_index][sample_index][pos_ceil + buflimit] * frac;
    }
}
} // namespace thl::dsp::granular