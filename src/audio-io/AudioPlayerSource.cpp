#include <tanh/audio-io/AudioPlayerSource.h>
#include <cstring>

namespace thl {

AudioPlayerSource::AudioPlayerSource() = default;

AudioPlayerSource::~AudioPlayerSource() {
    unload_file();
}

bool AudioPlayerSource::load_file(const std::string& file_path,
                                  uint32_t output_channels,
                                  uint32_t output_sample_rate) {
    if (file_path.empty() || output_channels == 0 || output_sample_rate == 0) { return false; }

    std::scoped_lock lock(m_state_mutex);
    m_playing.store(false, std::memory_order_release);
    atomic_store(m_data_source, std::shared_ptr<audio_io::DataSource>(nullptr));
    m_loaded.store(false, std::memory_order_release);

    m_file_path = file_path;
    m_memory_data = nullptr;
    m_memory_size = 0;
    m_channels = output_channels;
    m_sample_rate = output_sample_rate;

    bool loaded = rebuild_data_source(m_channels, m_sample_rate, 0);
    m_loaded.store(loaded, std::memory_order_release);
    if (!loaded) {
        m_file_path.clear();
        m_channels = 0;
        m_sample_rate = 0;
    }
    return loaded;
}

bool AudioPlayerSource::load_from_memory(const void* data,
                                         size_t size,
                                         uint32_t output_channels,
                                         uint32_t output_sample_rate) {
    if (data == nullptr || size == 0 || output_channels == 0 || output_sample_rate == 0) {
        return false;
    }

    std::scoped_lock lock(m_state_mutex);
    m_playing.store(false, std::memory_order_release);
    atomic_store(m_data_source, std::shared_ptr<audio_io::DataSource>(nullptr));
    m_loaded.store(false, std::memory_order_release);

    m_file_path.clear();
    m_memory_data = data;
    m_memory_size = size;
    m_channels = output_channels;
    m_sample_rate = output_sample_rate;

    bool loaded = rebuild_data_source(m_channels, m_sample_rate, 0);
    m_loaded.store(loaded, std::memory_order_release);
    if (!loaded) {
        m_memory_data = nullptr;
        m_memory_size = 0;
        m_channels = 0;
        m_sample_rate = 0;
    }
    return loaded;
}

void AudioPlayerSource::unload_file() {
    m_playing.store(false, std::memory_order_release);

    std::scoped_lock lock(m_state_mutex);
    atomic_store(m_data_source, std::shared_ptr<audio_io::DataSource>(nullptr));
    m_loaded.store(false, std::memory_order_release);
    m_channels = 0;
    m_sample_rate = 0;
    m_file_path.clear();
    m_memory_data = nullptr;
    m_memory_size = 0;
}

void AudioPlayerSource::play() {
    if (m_loaded.load(std::memory_order_acquire)) {
        m_fade_in_remaining.store(m_fade_enabled ? k_fade_samples : 0, std::memory_order_release);
        m_stop_requested.store(false, std::memory_order_release);
        m_playing.store(true, std::memory_order_release);
    }
}

void AudioPlayerSource::pause() {
    m_playing.store(false, std::memory_order_release);
}

void AudioPlayerSource::stop() {
    m_playing.store(false, std::memory_order_release);
    m_stop_requested.store(false, std::memory_order_release);
    m_fade_out_counter = 0;
    auto ds = atomic_load(m_data_source);
    if (ds) { ds->seek(0); }
}

void AudioPlayerSource::request_stop() {
    if (m_playing.load(std::memory_order_acquire)) {
        m_stop_requested.store(true, std::memory_order_release);
    }
}

void AudioPlayerSource::seek_to_frame(uint64_t frame) {
    auto ds = atomic_load(m_data_source);
    if (ds) { ds->seek(frame); }
}

uint64_t AudioPlayerSource::get_current_frame() const {
    auto ds = atomic_load(m_data_source);
    if (!ds) { return 0; }
    return ds->get_cursor();
}

uint64_t AudioPlayerSource::get_total_frames() const {
    auto ds = atomic_load(m_data_source);
    if (!ds) { return 0; }
    return ds->get_total_frames();
}

void AudioPlayerSource::set_finished_callback(FinishedCallback callback) {
    auto ptr = callback ? std::make_shared<FinishedCallback>(std::move(callback)) : nullptr;
    atomic_store(m_finished_callback, std::move(ptr));
}

void AudioPlayerSource::prepare_to_play(uint32_t sample_rate, uint32_t /*bufferSize*/) {
    if (sample_rate == 0) { return; }

    std::scoped_lock lock(m_state_mutex);
    if (!m_loaded.load(std::memory_order_acquire)) { return; }
    if (m_file_path.empty() && m_memory_data == nullptr) { return; }

    uint64_t initial_frame = 0;
    auto ds = atomic_load(m_data_source);
    if (ds && ds->get_sample_rate() == sample_rate && ds->get_channel_count() == m_channels) {
        initial_frame = ds->get_cursor();
    }

    bool was_playing = m_playing.load(std::memory_order_acquire);
    m_playing.store(false, std::memory_order_release);

    if (rebuild_data_source(m_channels, sample_rate, initial_frame)) {
        m_sample_rate = sample_rate;
        if (was_playing) { m_playing.store(true, std::memory_order_release); }
    }
}

void AudioPlayerSource::process(float* output_buffer,
                                const float* /*inputBuffer*/,
                                uint32_t frame_count,
                                uint32_t /*numInputChannels*/,
                                uint32_t num_output_channels) {
    if (!output_buffer || num_output_channels == 0) { return; }
    if (!m_loaded.load(std::memory_order_acquire) || !m_playing.load(std::memory_order_acquire)) {
        return;
    }

    auto ds = atomic_load(m_data_source);
    if (!ds) { return; }

    if (num_output_channels != ds->get_channel_count()) { return; }

    uint64_t frames_read = ds->read_pcm_frames(output_buffer, frame_count);

    if (frames_read < frame_count) {
        std::memset(output_buffer + frames_read * num_output_channels,
                    0,
                    (frame_count - frames_read) * num_output_channels * sizeof(float));
    }

    // --- Micro fade-in (applied after every play()) ---
    uint32_t fade_in = m_fade_in_remaining.load(std::memory_order_acquire);
    if (fade_in > 0) {
        const uint32_t fade_start = k_fade_samples - fade_in;
        const uint32_t to_fade = std::min(fade_in, static_cast<uint32_t>(frames_read));
        for (uint32_t i = 0; i < to_fade; ++i) {
            const float gain =
                static_cast<float>(fade_start + i) / static_cast<float>(k_fade_samples);
            for (uint32_t ch = 0; ch < num_output_channels; ++ch) {
                output_buffer[i * num_output_channels + ch] *= gain;
            }
        }
        m_fade_in_remaining.store(fade_in - to_fade, std::memory_order_release);
    }

    // --- Micro fade-out (request_stop) ---
    if (m_stop_requested.load(std::memory_order_acquire)) {
        if (m_fade_out_counter == 0) { m_fade_out_counter = k_fade_samples; }
        for (uint32_t i = 0; i < static_cast<uint32_t>(frames_read); ++i) {
            float gain;
            if (m_fade_out_counter > 0) {
                --m_fade_out_counter;
                gain = static_cast<float>(m_fade_out_counter) / static_cast<float>(k_fade_samples);
            } else {
                gain = 0.0f;
            }
            for (uint32_t ch = 0; ch < num_output_channels; ++ch) {
                output_buffer[i * num_output_channels + ch] *= gain;
            }
        }
        if (m_fade_out_counter == 0) {
            m_playing.store(false, std::memory_order_release);
            m_stop_requested.store(false, std::memory_order_release);
        }
        return;
    }

    if (frames_read < frame_count) {
        m_playing.store(false, std::memory_order_release);
        auto callback = atomic_load(m_finished_callback);
        if (callback) { (*callback)(); }
    }
}

void AudioPlayerSource::release_resources() {
    unload_file();
}

bool AudioPlayerSource::rebuild_data_source(uint32_t decoded_channels,
                                            uint32_t decoded_sample_rate,
                                            uint64_t initial_frame) {
    if (decoded_channels == 0 || decoded_sample_rate == 0) { return false; }

    std::shared_ptr<audio_io::DataSource> ds;

    if (m_memory_data != nullptr && m_memory_size > 0) {
        ds = std::make_shared<audio_io::DataSource>(
            m_loader.load_data_source_from_memory(m_memory_data,
                                                  m_memory_size,
                                                  static_cast<double>(decoded_sample_rate),
                                                  decoded_channels));
    } else if (!m_file_path.empty()) {
        ds = std::make_shared<audio_io::DataSource>(
            m_loader.load_data_source_from_file(m_file_path,
                                                static_cast<double>(decoded_sample_rate),
                                                decoded_channels));
    } else {
        return false;
    }

    if (!ds->is_valid()) { return false; }

    if (initial_frame > 0) { ds->seek(initial_frame); }

    atomic_store(m_data_source, std::move(ds));
    return true;
}

}  // namespace thl
