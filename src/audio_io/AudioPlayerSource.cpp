#include <tanh/audio_io/AudioPlayerSource.h>
#include "miniaudio.h"
#include <cstring>

namespace thl {

struct AudioPlayerSource::Impl {
    struct PlaybackState {
        ma_resource_manager resourceManager{};
        ma_resource_manager_data_source dataSource{};
        bool resourceManagerInitialised = false;
        bool dataSourceInitialised = false;
        uint32_t decodedChannels = 0;
        uint32_t decodedSampleRate = 0;

        ~PlaybackState() {
            if (dataSourceInitialised) {
                ma_resource_manager_data_source_uninit(&dataSource);
                dataSourceInitialised = false;
            }
            if (resourceManagerInitialised) {
                ma_resource_manager_uninit(&resourceManager);
                resourceManagerInitialised = false;
            }
        }
    };

    std::shared_ptr<PlaybackState> activeState;
};

AudioPlayerSource::AudioPlayerSource() : m_impl(std::make_unique<Impl>()) {}

AudioPlayerSource::~AudioPlayerSource() {
    unloadFile();
}

bool AudioPlayerSource::loadFile(const std::string& filePath,
                                 uint32_t outputChannels,
                                 uint32_t outputSampleRate) {
    if (filePath.empty() || outputChannels == 0 || outputSampleRate == 0) {
        return false;
    }

    std::scoped_lock lock(m_stateMutex);
    m_playing.store(false, std::memory_order_release);
    std::atomic_store_explicit(&m_impl->activeState,
                               std::shared_ptr<Impl::PlaybackState>{},
                               std::memory_order_release);
    m_loaded.store(false, std::memory_order_release);

    m_filePath = filePath;
    m_channels = outputChannels;
    m_sampleRate = outputSampleRate;

    bool loaded = rebuildDataSource(m_channels, m_sampleRate, 0);
    m_loaded.store(loaded, std::memory_order_release);
    if (!loaded) {
        m_filePath.clear();
        m_channels = 0;
        m_sampleRate = 0;
    }
    return loaded;
}

void AudioPlayerSource::unloadFile() {
    m_playing.store(false, std::memory_order_release);

    std::scoped_lock lock(m_stateMutex);
    std::atomic_store_explicit(&m_impl->activeState,
                               std::shared_ptr<Impl::PlaybackState>{},
                               std::memory_order_release);
    m_loaded.store(false, std::memory_order_release);
    m_channels = 0;
    m_sampleRate = 0;
    m_filePath.clear();
}

void AudioPlayerSource::play() {
    if (m_loaded.load(std::memory_order_acquire)) {
        m_playing.store(true, std::memory_order_release);
    }
}

void AudioPlayerSource::pause() {
    m_playing.store(false, std::memory_order_release);
}

void AudioPlayerSource::stop() {
    m_playing.store(false, std::memory_order_release);
    auto state = std::atomic_load_explicit(&m_impl->activeState,
                                           std::memory_order_acquire);
    if (state) { ma_data_source_seek_to_pcm_frame(&state->dataSource, 0); }
}

void AudioPlayerSource::seekToFrame(uint64_t frame) {
    auto state = std::atomic_load_explicit(&m_impl->activeState,
                                           std::memory_order_acquire);
    if (state) { ma_data_source_seek_to_pcm_frame(&state->dataSource, frame); }
}

uint64_t AudioPlayerSource::getCurrentFrame() const {
    auto state = std::atomic_load_explicit(&m_impl->activeState,
                                           std::memory_order_acquire);
    if (!state) return 0;

    ma_uint64 cursor = 0;
    ma_data_source_get_cursor_in_pcm_frames(&state->dataSource, &cursor);
    return cursor;
}

uint64_t AudioPlayerSource::getTotalFrames() const {
    auto state = std::atomic_load_explicit(&m_impl->activeState,
                                           std::memory_order_acquire);
    if (!state) return 0;

    ma_uint64 length = 0;
    ma_data_source_get_length_in_pcm_frames(&state->dataSource, &length);
    return length;
}

void AudioPlayerSource::setFinishedCallback(FinishedCallback callback) {
    auto ptr = callback
                   ? std::make_shared<FinishedCallback>(std::move(callback))
                   : nullptr;
    std::atomic_store_explicit(&m_finishedCallback,
                               std::move(ptr),
                               std::memory_order_release);
}

void AudioPlayerSource::prepareToPlay(uint32_t sampleRate,
                                      uint32_t /*bufferSize*/) {
    if (sampleRate == 0) return;

    std::scoped_lock lock(m_stateMutex);
    if (!m_loaded.load(std::memory_order_acquire) || m_filePath.empty()) return;

    uint64_t initialFrame = 0;
    auto activeState = std::atomic_load_explicit(&m_impl->activeState,
                                                 std::memory_order_acquire);
    if (activeState && activeState->decodedSampleRate == sampleRate &&
        activeState->decodedChannels == m_channels) {
        ma_data_source_get_cursor_in_pcm_frames(&activeState->dataSource,
                                                &initialFrame);
    }

    bool wasPlaying = m_playing.load(std::memory_order_acquire);
    m_playing.store(false, std::memory_order_release);

    if (rebuildDataSource(m_channels, sampleRate, initialFrame)) {
        m_sampleRate = sampleRate;
        if (wasPlaying) { m_playing.store(true, std::memory_order_release); }
    }
}

void AudioPlayerSource::process(float* outputBuffer,
                                const float* /*inputBuffer*/,
                                uint32_t frameCount,
                                uint32_t /*numInputChannels*/,
                                uint32_t numOutputChannels) {
    if (!outputBuffer || numOutputChannels == 0) { return; }
    if (!m_loaded.load(std::memory_order_acquire) ||
        !m_playing.load(std::memory_order_acquire)) {
        return;
    }

    auto state = std::atomic_load_explicit(&m_impl->activeState,
                                           std::memory_order_acquire);
    if (!state) return;

    if (numOutputChannels != state->decodedChannels) { return; }

    ma_uint64 framesRead = 0;
    ma_data_source_read_pcm_frames(&state->dataSource,
                                   outputBuffer,
                                   frameCount,
                                   &framesRead);

    if (framesRead < frameCount) {
        std::memset(
            outputBuffer + framesRead * numOutputChannels,
            0,
            (frameCount - framesRead) * numOutputChannels * sizeof(float));
    }

    if (framesRead < frameCount) {
        m_playing.store(false, std::memory_order_release);
        auto callback = std::atomic_load_explicit(&m_finishedCallback,
                                                  std::memory_order_acquire);
        if (callback) { (*callback)(); }
    }
}

void AudioPlayerSource::releaseResources() {
    unloadFile();
}

bool AudioPlayerSource::rebuildDataSource(uint32_t decodedChannels,
                                          uint32_t decodedSampleRate,
                                          uint64_t initialFrame) {
    if (m_filePath.empty() || decodedChannels == 0 || decodedSampleRate == 0) {
        return false;
    }

    auto newState = std::make_shared<Impl::PlaybackState>();

    ma_resource_manager_config rmConfig = ma_resource_manager_config_init();
    rmConfig.decodedFormat = ma_format_f32;
    rmConfig.decodedChannels = decodedChannels;
    rmConfig.decodedSampleRate = decodedSampleRate;

    ma_result result =
        ma_resource_manager_init(&rmConfig, &newState->resourceManager);
    if (result != MA_SUCCESS) return false;
    newState->resourceManagerInitialised = true;
    newState->decodedChannels = decodedChannels;
    newState->decodedSampleRate = decodedSampleRate;

    // IMPORTANT:
    // We intentionally request both:
    // - Force miniaudio to produce already-decoded PCM in the configured
    //   decoded format/channels/sample-rate from rmConfig above.
    // - This keeps channel/sample-rate adaptation out of the realtime
    //   process() path.
    // Tradeoff:
    // - WAIT_INIT can block the caller thread of rebuildDataSource()
    //   (e.g. prepareToPlay() call path), so this is not "free".
    // If we choose to call using the internal thread then we might need to
    // attach a notification callback.
    ma_uint32 flags = MA_RESOURCE_MANAGER_DATA_SOURCE_FLAG_DECODE |
                      MA_RESOURCE_MANAGER_DATA_SOURCE_FLAG_WAIT_INIT;
    result = ma_resource_manager_data_source_init(&newState->resourceManager,
                                                  m_filePath.c_str(),
                                                  flags,
                                                  nullptr,
                                                  &newState->dataSource);
    if (result != MA_SUCCESS) return false;
    newState->dataSourceInitialised = true;

    if (initialFrame > 0) {
        ma_data_source_seek_to_pcm_frame(&newState->dataSource, initialFrame);
    }

    std::atomic_store_explicit(&m_impl->activeState,
                               std::move(newState),
                               std::memory_order_release);
    return true;
}

}  // namespace thl
