#include <tanh/audio_io/AudioPlayerSource.h>
#include <cstring>

namespace thl {

AudioPlayerSource::AudioPlayerSource() {
    ma_resource_manager_config config = ma_resource_manager_config_init();
    config.decodedFormat = ma_format_f32;

    ma_result result = ma_resource_manager_init(&config, &m_resourceManager);
    m_resourceManagerInitialised = (result == MA_SUCCESS);
}

AudioPlayerSource::~AudioPlayerSource() {
    unloadFile();
    if (m_resourceManagerInitialised) {
        ma_resource_manager_uninit(&m_resourceManager);
        m_resourceManagerInitialised = false;
    }
}

bool AudioPlayerSource::loadFile(const std::string& filePath,
                                 ma_uint32 outputChannels,
                                 ma_uint32 outputSampleRate) {
    if (!m_resourceManagerInitialised) return false;

    unloadFile();

    ma_uint32 flags = MA_RESOURCE_MANAGER_DATA_SOURCE_FLAG_STREAM |
                      MA_RESOURCE_MANAGER_DATA_SOURCE_FLAG_WAIT_INIT;

    ma_result result = ma_resource_manager_data_source_init(&m_resourceManager,
                                                            filePath.c_str(),
                                                            flags,
                                                            nullptr,
                                                            &m_dataSource);

    if (result != MA_SUCCESS) { return false; }

    m_loaded = true;
    m_channels = outputChannels;
    m_sampleRate = outputSampleRate;

    return true;
}

void AudioPlayerSource::unloadFile() {
    if (m_loaded) {
        m_playing.store(false, std::memory_order_release);
        ma_resource_manager_data_source_uninit(&m_dataSource);
        m_loaded = false;
        m_channels = 0;
        m_sampleRate = 0;
    }
}

void AudioPlayerSource::play() {
    if (m_loaded) { m_playing.store(true, std::memory_order_release); }
}

void AudioPlayerSource::pause() {
    m_playing.store(false, std::memory_order_release);
}

void AudioPlayerSource::stop() {
    m_playing.store(false, std::memory_order_release);
    if (m_loaded) { ma_data_source_seek_to_pcm_frame(&m_dataSource, 0); }
}

void AudioPlayerSource::seekToFrame(ma_uint64 frame) {
    if (m_loaded) { ma_data_source_seek_to_pcm_frame(&m_dataSource, frame); }
}

ma_uint64 AudioPlayerSource::getCurrentFrame() const {
    if (!m_loaded) return 0;

    ma_uint64 cursor = 0;
    ma_data_source_get_cursor_in_pcm_frames(
        const_cast<ma_resource_manager_data_source*>(&m_dataSource),
        &cursor);
    return cursor;
}

ma_uint64 AudioPlayerSource::getTotalFrames() const {
    if (!m_loaded) return 0;

    ma_uint64 length = 0;
    ma_data_source_get_length_in_pcm_frames(
        const_cast<ma_resource_manager_data_source*>(&m_dataSource),
        &length);
    return length;
}

void AudioPlayerSource::setFinishedCallback(FinishedCallback callback) {
    m_finishedCallback = std::move(callback);
}

void AudioPlayerSource::process(float* outputBuffer,
                                const float* /*inputBuffer*/,
                                ma_uint32 frameCount,
                                ma_uint32 numChannels) {
    if (!m_loaded || !m_playing.load(std::memory_order_acquire)) { return; }

    ma_uint64 framesRead = 0;
    ma_data_source_read_pcm_frames(&m_dataSource,
                                   outputBuffer,
                                   frameCount,
                                   &framesRead);

    if (framesRead < frameCount) {
        std::memset(outputBuffer + framesRead * numChannels,
                    0,
                    (frameCount - framesRead) * numChannels * sizeof(float));

        m_playing.store(false, std::memory_order_release);
        if (m_finishedCallback) { m_finishedCallback(); }
    }
}

void AudioPlayerSource::releaseResources() {
    unloadFile();
}

}  // namespace thl
