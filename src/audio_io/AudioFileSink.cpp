#include <tanh/audio_io/AudioFileSink.h>
#include "miniaudio.h"
#include <cmath>

namespace thl {

struct AudioFileSink::Impl {
    ma_encoder encoder{};
    uint32_t encoderChannels = 0;
};

namespace {
ma_encoding_format toMiniaudioFormat(AudioEncodingFormat format) {
    switch (format) {
        case AudioEncodingFormat::WAV: return ma_encoding_format_wav;
        case AudioEncodingFormat::FLAC: return ma_encoding_format_flac;
    }
    return ma_encoding_format_wav;
}
}  // namespace

AudioFileSink::AudioFileSink() : m_impl(std::make_unique<Impl>()) {}

AudioFileSink::~AudioFileSink() {
    closeFile();
}

bool AudioFileSink::openFile(const std::string& filePath,
                             uint32_t channels,
                             uint32_t sampleRate,
                             AudioEncodingFormat format) {
    closeFile();

    ma_encoder_config config = ma_encoder_config_init(toMiniaudioFormat(format),
                                                      ma_format_f32,
                                                      channels,
                                                      sampleRate);

    ma_result result =
        ma_encoder_init_file(filePath.c_str(), &config, &m_impl->encoder);

    m_open = (result == MA_SUCCESS);
    m_impl->encoderChannels = m_open ? channels : 0;
    m_framesWritten.store(0, std::memory_order_release);
    return m_open;
}

void AudioFileSink::closeFile() {
    if (m_open) {
        m_recording.store(false, std::memory_order_release);
        ma_encoder_uninit(&m_impl->encoder);
        m_open = false;
        m_impl->encoderChannels = 0;
    }
}

void AudioFileSink::startRecording() {
    if (m_open) {
        m_peakLevel.store(0.0f, std::memory_order_release);
        m_recording.store(true, std::memory_order_release);
    }
}

void AudioFileSink::stopRecording() {
    m_recording.store(false, std::memory_order_release);
}

void AudioFileSink::process(float* /*outputBuffer*/,
                            const float* inputBuffer,
                            uint32_t frameCount,
                            uint32_t numInputChannels,
                            uint32_t /*numOutputChannels*/
) {
    if (!inputBuffer || numInputChannels == 0) { return; }
    if (!m_open || !m_recording.load(std::memory_order_acquire)) { return; }
    if (numInputChannels != m_impl->encoderChannels) { return; }

    ma_uint64 framesWritten = 0;
    ma_encoder_write_pcm_frames(&m_impl->encoder,
                                inputBuffer,
                                frameCount,
                                &framesWritten);
    m_framesWritten.fetch_add(framesWritten, std::memory_order_relaxed);

    // Compute peak amplitude for this block
    float peak = 0.0f;
    const uint32_t totalSamples = frameCount * numInputChannels;
    for (uint32_t i = 0; i < totalSamples; ++i) {
        float absVal = std::fabs(inputBuffer[i]);
        if (absVal > peak) peak = absVal;
    }
    m_peakLevel.store(peak, std::memory_order_release);
}

void AudioFileSink::releaseResources() {
    closeFile();
}

}  // namespace thl
