#include <tanh/audio_io/AudioDeviceManager.h>
#include <algorithm>
#include <cstring>
#include <vector>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace {
struct EnumUserData {
    std::vector<thl::AudioDeviceInfo>* captureDevices;
    std::vector<thl::AudioDeviceInfo>* playbackDevices;
};

ma_bool32 enumCallback(ma_context* /*pContext*/,
                       ma_device_type deviceType,
                       const ma_device_info* pInfo,
                       void* pUserData) {
    auto* userData = static_cast<EnumUserData*>(pUserData);

    thl::AudioDeviceInfo info;
    info.name = pInfo->name;
    info.deviceID = pInfo->id;
    info.deviceType = deviceType;

    if (deviceType == ma_device_type_capture && userData->captureDevices) {
        userData->captureDevices->push_back(std::move(info));
    } else if (deviceType == ma_device_type_playback &&
               userData->playbackDevices) {
        userData->playbackDevices->push_back(std::move(info));
    }

    return MA_TRUE;
}
}  // namespace

namespace thl {

AudioDeviceManager::AudioDeviceManager() {
    ma_context_config ctxConfig = ma_context_config_init();

#if defined(__APPLE__) && TARGET_OS_IPHONE
    ctxConfig.coreaudio.sessionCategory =
        ma_ios_session_category_play_and_record;
    ctxConfig.coreaudio.sessionCategoryOptions =
        ma_ios_session_category_option_default_to_speaker |
        ma_ios_session_category_option_allow_bluetooth;
#endif

    ma_result result = ma_context_init(nullptr, 0, &ctxConfig, &m_context);
    m_contextInitialised = (result == MA_SUCCESS);
}

AudioDeviceManager::~AudioDeviceManager() {
    shutdown();
    if (m_contextInitialised) {
        ma_context_uninit(&m_context);
        m_contextInitialised = false;
    }
}

void AudioDeviceManager::shutdown() {
    stop();
    if (m_deviceInitialised) {
        ma_device_uninit(&m_device);
        m_deviceInitialised = false;
    }
}

void AudioDeviceManager::populateSampleRates(
    std::vector<AudioDeviceInfo>& devices) const {
    for (auto& info : devices) {
        ma_device_info deviceInfo;
        ma_result result =
            ma_context_get_device_info(const_cast<ma_context*>(&m_context),
                                       info.deviceType,
                                       info.deviceIDPtr(),
                                       &deviceInfo);

        std::vector<ma_uint32> rates;
        if (result == MA_SUCCESS) {
            for (ma_uint32 i = 0; i < deviceInfo.nativeDataFormatCount; i++) {
                ma_uint32 rate = deviceInfo.nativeDataFormats[i].sampleRate;
                if (rate > 0) {
                    if (std::find(rates.begin(), rates.end(), rate) ==
                        rates.end()) {
                        rates.push_back(rate);
                    }
                }
            }
        }

        if (rates.empty()) { rates = {22050, 44100, 48000, 96000}; }

        std::sort(rates.begin(), rates.end());
        info.sampleRates = std::move(rates);
    }
}

std::vector<AudioDeviceInfo> AudioDeviceManager::enumerateInputDevices() const {
    if (!m_contextInitialised) return {};

    std::vector<AudioDeviceInfo> captureDevices;
    EnumUserData userData{&captureDevices, nullptr};
    ma_context_enumerate_devices(const_cast<ma_context*>(&m_context),
                                 enumCallback,
                                 &userData);

    const_cast<AudioDeviceManager*>(this)->populateSampleRates(captureDevices);
    return captureDevices;
}

std::vector<AudioDeviceInfo> AudioDeviceManager::enumerateOutputDevices()
    const {
    if (!m_contextInitialised) return {};

    std::vector<AudioDeviceInfo> playbackDevices;
    EnumUserData userData{nullptr, &playbackDevices};
    ma_context_enumerate_devices(const_cast<ma_context*>(&m_context),
                                 enumCallback,
                                 &userData);

    const_cast<AudioDeviceManager*>(this)->populateSampleRates(playbackDevices);
    return playbackDevices;
}

bool AudioDeviceManager::initialise(const AudioDeviceInfo* inputDevice,
                                    const AudioDeviceInfo* outputDevice,
                                    ma_uint32 sampleRate,
                                    ma_uint32 bufferSizeInFrames,
                                    ma_uint32 numChannels) {
    if (!m_contextInitialised) return false;

    shutdown();

    m_sampleRate = sampleRate;
    m_bufferSize = bufferSizeInFrames;
    m_numChannels = numChannels;

    ma_device_config devConfig = ma_device_config_init(ma_device_type_duplex);

    if (inputDevice) {
        devConfig.capture.pDeviceID = inputDevice->deviceIDPtr();
    }
    if (outputDevice) {
        devConfig.playback.pDeviceID = outputDevice->deviceIDPtr();
    }

    devConfig.capture.format = ma_format_f32;
    devConfig.capture.channels = numChannels;
    devConfig.playback.format = ma_format_f32;
    devConfig.playback.channels = numChannels;
    devConfig.sampleRate = sampleRate;
    devConfig.periodSizeInFrames = bufferSizeInFrames;
    devConfig.dataCallback = dataCallback;
    devConfig.notificationCallback = notificationCallback;
    devConfig.pUserData = this;

    ma_result result = ma_device_init(&m_context, &devConfig, &m_device);
    if (result != MA_SUCCESS) { return false; }

    m_deviceInitialised = true;

    m_callbacks.read([&](const auto& callbacks) {
        for (auto* callback : callbacks) {
            callback->prepareToPlay(m_device.sampleRate, bufferSizeInFrames);
        }
    });

    return true;
}

bool AudioDeviceManager::start() {
    if (!m_deviceInitialised || m_running) return false;

    ma_result result = ma_device_start(&m_device);
    if (result != MA_SUCCESS) { return false; }

    m_running = true;
    return true;
}

void AudioDeviceManager::stop() {
    if (!m_running) return;

    ma_device_stop(&m_device);
    m_running = false;
    m_audioThreadRegistered.store(false, std::memory_order_relaxed);

    m_callbacks.read([](const auto& callbacks) {
        for (auto* callback : callbacks) { callback->releaseResources(); }
    });
}

void AudioDeviceManager::addCallback(AudioIODeviceCallback* callback) {
    if (!callback) return;

    bool added = false;
    m_callbacks.update([&](auto& callbacks) {
        if (std::find(callbacks.begin(), callbacks.end(), callback) ==
            callbacks.end()) {
            callbacks.push_back(callback);
            added = true;
        }
    });

    if (added && m_deviceInitialised) {
        callback->prepareToPlay(m_device.sampleRate, m_bufferSize);
    }
}

void AudioDeviceManager::removeCallback(AudioIODeviceCallback* callback) {
    AudioIODeviceCallback* removed = nullptr;
    m_callbacks.update([&](auto& callbacks) {
        auto it = std::find(callbacks.begin(), callbacks.end(), callback);
        if (it != callbacks.end()) {
            removed = *it;
            callbacks.erase(it);
        }
    });

    if (removed) { removed->releaseResources(); }
}

void AudioDeviceManager::setDeviceNotificationCallback(
    DeviceNotificationCallback callback) {
    m_notificationCallback = std::move(callback);
}

void AudioDeviceManager::dataCallback(ma_device* pDevice,
                                      void* pOutput,
                                      const void* pInput,
                                      ma_uint32 frameCount) {
    auto* self = static_cast<AudioDeviceManager*>(pDevice->pUserData);
    self->processCallbacks(static_cast<float*>(pOutput),
                           static_cast<const float*>(pInput),
                           frameCount);
}

void AudioDeviceManager::notificationCallback(
    const ma_device_notification* pNotification) {
    auto* self =
        static_cast<AudioDeviceManager*>(pNotification->pDevice->pUserData);
    if (self->m_notificationCallback) {
        self->m_notificationCallback(pNotification->type);
    }
}

void AudioDeviceManager::processCallbacks(float* output,
                                          const float* input,
                                          ma_uint32 frameCount) {
    // Register audio thread with RCU on first callback (not RT-safe, but only
    // happens once)
    if (!m_audioThreadRegistered.load(std::memory_order_relaxed)) [[unlikely]] {
        m_callbacks.register_reader_thread();
        m_audioThreadRegistered.store(true, std::memory_order_relaxed);
    }

    std::memset(output, 0, frameCount * m_numChannels * sizeof(float));

    m_callbacks.read([&](const auto& callbacks) {
        for (auto* callback : callbacks) {
            callback->process(output, input, frameCount, m_numChannels);
        }
    });
}

}  // namespace thl
