#include <tanh/audio_io/AudioFileSink.h>

namespace thl {

AudioFileSink::AudioFileSink() = default;

AudioFileSink::~AudioFileSink() {
    closeFile();
}

bool AudioFileSink::openFile(const std::string& filePath,
                             ma_uint32 channels,
                             ma_uint32 sampleRate,
                             ma_encoding_format format) {
    closeFile();

    ma_encoder_config config =
        ma_encoder_config_init(format, ma_format_f32, channels, sampleRate);

    ma_result result =
        ma_encoder_init_file(filePath.c_str(), &config, &m_encoder);

    m_open = (result == MA_SUCCESS);
    m_framesWritten.store(0, std::memory_order_release);
    return m_open;
}

void AudioFileSink::closeFile() {
    if (m_open) {
        m_recording.store(false, std::memory_order_release);
        ma_encoder_uninit(&m_encoder);
        m_open = false;
    }
}

void AudioFileSink::startRecording() {
    if (m_open) { m_recording.store(true, std::memory_order_release); }
}

void AudioFileSink::stopRecording() {
    m_recording.store(false, std::memory_order_release);
}

void AudioFileSink::process(float* /*outputBuffer*/,
                            const float* inputBuffer,
                            ma_uint32 frameCount,
                            ma_uint32 numInputChannels,
                            ma_uint32 /*numOutputChannels*/
) {
    if (!inputBuffer || numInputChannels == 0) { return; }
    if (!m_open || !m_recording.load(std::memory_order_acquire)) { return; }

    ma_uint64 framesWritten = 0;
    ma_encoder_write_pcm_frames(&m_encoder,
                                inputBuffer,
                                frameCount,
                                &framesWritten);
    m_framesWritten.fetch_add(framesWritten, std::memory_order_relaxed);
}

void AudioFileSink::releaseResources() {
    closeFile();
}

}  // namespace thl
