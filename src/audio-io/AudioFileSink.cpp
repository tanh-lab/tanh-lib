#include <tanh/audio-io/AudioFileSink.h>
#include "miniaudio.h"
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>

namespace thl {

struct AudioFileSink::Impl {
    ma_encoder m_encoder{};
    uint32_t m_encoder_channels = 0;
};

namespace {
ma_encoding_format to_miniaudio_format(AudioEncodingFormat format) {
    switch (format) {
        case AudioEncodingFormat::WAV: return ma_encoding_format_wav;
        case AudioEncodingFormat::FLAC: return ma_encoding_format_flac;
    }
    return ma_encoding_format_wav;
}
}  // namespace

AudioFileSink::AudioFileSink() : m_impl(std::make_unique<Impl>()) {}

AudioFileSink::~AudioFileSink() {
    close_file();
}

bool AudioFileSink::open_file(const std::string& file_path,
                              uint32_t channels,
                              uint32_t sample_rate,
                              AudioEncodingFormat format) {
    close_file();

    ma_encoder_config const config =
        ma_encoder_config_init(to_miniaudio_format(format), ma_format_f32, channels, sample_rate);

    ma_result const result = ma_encoder_init_file(file_path.c_str(), &config, &m_impl->m_encoder);

    m_open = (result == MA_SUCCESS);
    m_impl->m_encoder_channels = m_open ? channels : 0;
    m_frames_written.store(0, std::memory_order_release);
    return m_open;
}

void AudioFileSink::close_file() {
    if (m_open) {
        m_recording.store(false, std::memory_order_release);
        ma_encoder_uninit(&m_impl->m_encoder);
        m_open = false;
        m_impl->m_encoder_channels = 0;
    }
}

void AudioFileSink::start_recording() {
    if (m_open) {
        m_peak_level.store(0.0f, std::memory_order_release);
        m_recording.store(true, std::memory_order_release);
    }
}

void AudioFileSink::stop_recording() {
    m_recording.store(false, std::memory_order_release);
}

void AudioFileSink::process(float* /*outputBuffer*/,
                            const float* input_buffer,
                            uint32_t frame_count,
                            uint32_t num_input_channels,
                            uint32_t /*numOutputChannels*/
) {
    if (!input_buffer || num_input_channels == 0) { return; }
    if (!m_open || !m_recording.load(std::memory_order_acquire)) { return; }
    if (num_input_channels != m_impl->m_encoder_channels) { return; }

    ma_uint64 frames_written = 0;
    ma_encoder_write_pcm_frames(&m_impl->m_encoder, input_buffer, frame_count, &frames_written);
    m_frames_written.fetch_add(frames_written, std::memory_order_relaxed);

    // Compute peak amplitude for this block
    float peak = 0.0f;
    const uint32_t total_samples = frame_count * num_input_channels;
    for (uint32_t i = 0; i < total_samples; ++i) {
        float const abs_val = std::fabs(input_buffer[i]);
        if (abs_val > peak) { peak = abs_val; }
    }
    m_peak_level.store(peak, std::memory_order_release);
}

void AudioFileSink::release_resources() {
    close_file();
}

}  // namespace thl
