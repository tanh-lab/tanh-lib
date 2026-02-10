#include <tanh/audio_io/AudioPlayerSource.h>
#include <cstring>

namespace thl {

AudioPlayerSource::AudioPlayerSource() = default;

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
    std::atomic_store_explicit(&m_dataSource,
                               std::shared_ptr<audio_io::DataSource>{},
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
    std::atomic_store_explicit(&m_dataSource,
                               std::shared_ptr<audio_io::DataSource>{},
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
    auto ds =
        std::atomic_load_explicit(&m_dataSource, std::memory_order_acquire);
    if (ds) { ds->seek(0); }
}

void AudioPlayerSource::seekToFrame(uint64_t frame) {
    auto ds =
        std::atomic_load_explicit(&m_dataSource, std::memory_order_acquire);
    if (ds) { ds->seek(frame); }
}

uint64_t AudioPlayerSource::getCurrentFrame() const {
    auto ds =
        std::atomic_load_explicit(&m_dataSource, std::memory_order_acquire);
    if (!ds) return 0;
    return ds->get_cursor();
}

uint64_t AudioPlayerSource::getTotalFrames() const {
    auto ds =
        std::atomic_load_explicit(&m_dataSource, std::memory_order_acquire);
    if (!ds) return 0;
    return ds->get_total_frames();
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
    auto ds =
        std::atomic_load_explicit(&m_dataSource, std::memory_order_acquire);
    if (ds && ds->get_sample_rate() == sampleRate &&
        ds->get_channel_count() == m_channels) {
        initialFrame = ds->get_cursor();
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

    auto ds =
        std::atomic_load_explicit(&m_dataSource, std::memory_order_acquire);
    if (!ds) return;

    if (numOutputChannels != ds->get_channel_count()) { return; }

    uint64_t framesRead = ds->read_pcm_frames(outputBuffer, frameCount);

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

    auto ds = std::make_shared<audio_io::DataSource>(
        m_loader.load_data_source_from_file(
            m_filePath,
            static_cast<double>(decodedSampleRate),
            decodedChannels));

    if (!ds->is_valid()) return false;

    if (initialFrame > 0) { ds->seek(initialFrame); }

    std::atomic_store_explicit(&m_dataSource,
                               std::move(ds),
                               std::memory_order_release);
    return true;
}

}  // namespace thl
