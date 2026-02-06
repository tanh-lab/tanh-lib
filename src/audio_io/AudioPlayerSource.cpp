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

    // Get the source file's actual channel count
    ma_format sourceFormat;
    ma_uint32 sourceChannels;
    ma_uint32 sourceSampleRate;
    ma_data_source_get_data_format(&m_dataSource,
                                   &sourceFormat,
                                   &sourceChannels,
                                   &sourceSampleRate,
                                   nullptr,
                                   0);
    m_sourceChannels = sourceChannels;

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
        m_sourceChannels = 0;
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
                                ma_uint32 /*numInputChannels*/,
                                ma_uint32 numOutputChannels) {
    if (!outputBuffer || numOutputChannels == 0) { return; }
    if (!m_loaded || !m_playing.load(std::memory_order_acquire)) { return; }

    ma_uint64 framesRead = 0;

    // Handle mono-to-stereo upmixing
    if (m_sourceChannels == 1 && numOutputChannels > 1) {
        // Ensure temp buffer is large enough
        if (m_tempBuffer.size() < frameCount) {
            m_tempBuffer.resize(frameCount);
        }

        // Read mono frames into temp buffer
        ma_data_source_read_pcm_frames(&m_dataSource,
                                       m_tempBuffer.data(),
                                       frameCount,
                                       &framesRead);

        // Upmix: copy mono sample to all output channels
        // TODO: This is just a temporal solution
        // there must a better way to handle this
        for (ma_uint32 frame = 0; frame < framesRead; ++frame) {
            float sample = m_tempBuffer[frame];
            for (ma_uint32 ch = 0; ch < numOutputChannels; ++ch) {
                outputBuffer[frame * numOutputChannels + ch] = sample;
            }
        }

        // Zero remaining frames if we hit end of file
        if (framesRead < frameCount) {
            std::memset(
                outputBuffer + framesRead * numOutputChannels,
                0,
                (frameCount - framesRead) * numOutputChannels * sizeof(float));
        }
    } else {
        // Direct read for matching channel counts
        ma_data_source_read_pcm_frames(&m_dataSource,
                                       outputBuffer,
                                       frameCount,
                                       &framesRead);

        if (framesRead < frameCount) {
            std::memset(
                outputBuffer + framesRead * numOutputChannels,
                0,
                (frameCount - framesRead) * numOutputChannels * sizeof(float));
        }
    }

    if (framesRead < frameCount) {
        m_playing.store(false, std::memory_order_release);
        if (m_finishedCallback) { m_finishedCallback(); }
    }
}

void AudioPlayerSource::releaseResources() {
    unloadFile();
}

}  // namespace thl
