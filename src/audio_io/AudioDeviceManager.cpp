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

ma_uint32 resolveBufferSize(ma_device& device,
                            ma_uint32 requested,
                            const char* label) {
    ma_uint32 actualBufferSize = 0;
    if (device.playback.internalPeriodSizeInFrames > 0) {
        actualBufferSize = device.playback.internalPeriodSizeInFrames;
    } else if (device.capture.internalPeriodSizeInFrames > 0) {
        actualBufferSize = device.capture.internalPeriodSizeInFrames;
    }
    if (actualBufferSize == 0) {
        ma_log* log = ma_device_get_log(&device);
        if (log) {
            ma_log_postf(
                log,
                MA_LOG_LEVEL_ERROR,
                "AudioDeviceManager (%s): internalPeriodSizeInFrames is 0 "
                "(playback=%u, capture=%u). Falling back to requested "
                "bufferSizeInFrames=%u.",
                label,
                device.playback.internalPeriodSizeInFrames,
                device.capture.internalPeriodSizeInFrames,
                requested);
        }
        return requested;
    }
    return actualBufferSize;
}
}  // namespace

namespace thl {

AudioDeviceManager::AudioDeviceManager() {
    m_playbackUserData.manager = this;
    m_playbackUserData.role = DeviceRole::Playback;
    m_captureUserData.manager = this;
    m_captureUserData.role = DeviceRole::Capture;
    m_duplexUserData.manager = this;
    m_duplexUserData.role = DeviceRole::Duplex;

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
    stopPlayback();
    stopCapture();
    stopDuplex();

    if (m_playbackDeviceInitialised) {
        ma_device_uninit(&m_playbackDevice);
        m_playbackDeviceInitialised = false;
    }
    if (m_captureDeviceInitialised) {
        ma_device_uninit(&m_captureDevice);
        m_captureDeviceInitialised = false;
    }
    if (m_duplexDeviceInitialised) {
        ma_device_uninit(&m_duplexDevice);
        m_duplexDeviceInitialised = false;
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

        if (rates.empty()) {
            ma_log* log =
                ma_context_get_log(const_cast<ma_context*>(&m_context));
            if (log) {
                ma_log_postf(log,
                             MA_LOG_LEVEL_WARNING,
                             "AudioDeviceManager: No sample rates reported for "
                             "device '%s', using defaults",
                             info.name.c_str());
            }
            rates = {22050, 44100, 48000, 96000};
        }

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

bool AudioDeviceManager::tryInitialiseDevice(
    DeviceRole role,
    const AudioDeviceInfo* inputDevice,
    const AudioDeviceInfo* outputDevice,
    ma_uint32 sampleRate,
    ma_uint32 bufferSizeInFrames,
    ma_uint32 numInputChannels,
    ma_uint32 numOutputChannels) {
    ma_device* device = nullptr;
    DeviceUserData* userData = nullptr;
    ma_device_type deviceType = ma_device_type_playback;
    const char* label = nullptr;

    switch (role) {
        case DeviceRole::Playback:
            if (!outputDevice) { return false; }
            device = &m_playbackDevice;
            userData = &m_playbackUserData;
            deviceType = ma_device_type_playback;
            label = "playback";
            break;
        case DeviceRole::Capture:
            if (!inputDevice) { return false; }
            device = &m_captureDevice;
            userData = &m_captureUserData;
            deviceType = ma_device_type_capture;
            label = "capture";
            break;
        case DeviceRole::Duplex:
            if (!inputDevice || !outputDevice) { return false; }
            device = &m_duplexDevice;
            userData = &m_duplexUserData;
            deviceType = ma_device_type_duplex;
            label = "duplex";
            break;
    }

    ma_device_config devConfig = ma_device_config_init(deviceType);
    devConfig.sampleRate = sampleRate;
    devConfig.periodSizeInFrames = bufferSizeInFrames;
    devConfig.dataCallback = dataCallback;
    devConfig.notificationCallback = notificationCallback;
    devConfig.pUserData = userData;

    if (role == DeviceRole::Playback || role == DeviceRole::Duplex) {
        devConfig.playback.pDeviceID = outputDevice->deviceIDPtr();
        devConfig.playback.format = ma_format_f32;
        devConfig.playback.channels = numOutputChannels;
    }

    if (role == DeviceRole::Capture || role == DeviceRole::Duplex) {
        devConfig.capture.pDeviceID = inputDevice->deviceIDPtr();
        devConfig.capture.format = ma_format_f32;
        devConfig.capture.channels = numInputChannels;
    }

    if (ma_device_init(&m_context, &devConfig, device) != MA_SUCCESS) {
        return false;
    }

    switch (role) {
        case DeviceRole::Playback:
            m_playbackDeviceInitialised = true;
            m_numOutputChannels = device->playback.channels;
            m_sampleRate = device->sampleRate;
            m_playbackBufferSize =
                resolveBufferSize(*device, bufferSizeInFrames, label);
            break;
        case DeviceRole::Capture:
            m_captureDeviceInitialised = true;
            m_numInputChannels = device->capture.channels;
            if (!m_playbackDeviceInitialised) {
                m_sampleRate = device->sampleRate;
            }
            m_captureBufferSize =
                resolveBufferSize(*device, bufferSizeInFrames, label);
            break;
        case DeviceRole::Duplex:
            m_duplexDeviceInitialised = true;
            if (!m_playbackDeviceInitialised && !m_captureDeviceInitialised) {
                m_sampleRate = device->sampleRate;
            }
            m_duplexBufferSize =
                resolveBufferSize(*device, bufferSizeInFrames, label);
            break;
    }

    return true;
}

bool AudioDeviceManager::initialise(const AudioDeviceInfo* inputDevice,
                                    const AudioDeviceInfo* outputDevice,
                                    ma_uint32 sampleRate,
                                    ma_uint32 bufferSizeInFrames,
                                    ma_uint32 numInputChannels,
                                    ma_uint32 numOutputChannels) {
    if (!m_contextInitialised) return false;

    shutdown();

    m_sampleRate = sampleRate;
    m_bufferSize = bufferSizeInFrames;
    m_numInputChannels = inputDevice ? numInputChannels : 0;
    m_numOutputChannels = outputDevice ? numOutputChannels : 0;
    m_playbackBufferSize = bufferSizeInFrames;
    m_captureBufferSize = bufferSizeInFrames;
    m_duplexBufferSize = bufferSizeInFrames;

    bool anyInitialised = false;
    anyInitialised |= tryInitialiseDevice(DeviceRole::Playback,
                                          inputDevice,
                                          outputDevice,
                                          sampleRate,
                                          bufferSizeInFrames,
                                          numInputChannels,
                                          numOutputChannels);
    anyInitialised |= tryInitialiseDevice(DeviceRole::Capture,
                                          inputDevice,
                                          outputDevice,
                                          sampleRate,
                                          bufferSizeInFrames,
                                          numInputChannels,
                                          numOutputChannels);
    anyInitialised |= tryInitialiseDevice(DeviceRole::Duplex,
                                          inputDevice,
                                          outputDevice,
                                          sampleRate,
                                          bufferSizeInFrames,
                                          numInputChannels,
                                          numOutputChannels);

    if (!anyInitialised) { return false; }

    if (m_playbackDeviceInitialised) {
        m_bufferSize = m_playbackBufferSize;
    } else if (m_duplexDeviceInitialised) {
        m_bufferSize = m_duplexBufferSize;
    } else if (m_captureDeviceInitialised) {
        m_bufferSize = m_captureBufferSize;
    }

    auto prepareCallbacks = [](RCU<std::vector<AudioIODeviceCallback*>>& list,
                               ma_device& device,
                               ma_uint32 bufferSize) {
        list.read([&](const auto& callbacks) {
            for (auto* callback : callbacks) {
                callback->prepareToPlay(device.sampleRate, bufferSize);
            }
        });
    };

    if (m_playbackDeviceInitialised) {
        prepareCallbacks(m_playbackCallbacks,
                         m_playbackDevice,
                         m_playbackBufferSize);
    }
    if (m_captureDeviceInitialised) {
        prepareCallbacks(m_captureCallbacks,
                         m_captureDevice,
                         m_captureBufferSize);
    }
    if (m_duplexDeviceInitialised) {
        prepareCallbacks(m_duplexCallbacks, m_duplexDevice, m_duplexBufferSize);
    }

    return true;
}

bool AudioDeviceManager::startPlayback() {
    if (!m_playbackDeviceInitialised || m_playbackRunning) return false;

    ma_result result = ma_device_start(&m_playbackDevice);
    if (result != MA_SUCCESS) { return false; }

    m_playbackRunning = true;
    return true;
}

void AudioDeviceManager::stopPlayback() {
    if (!m_playbackRunning) return;

    ma_device_stop(&m_playbackDevice);
    m_playbackRunning = false;
    m_playbackAudioThreadRegistered.store(false, std::memory_order_relaxed);

    m_playbackCallbacks.read([](const auto& callbacks) {
        for (auto* callback : callbacks) { callback->releaseResources(); }
    });
}

bool AudioDeviceManager::startCapture() {
    if (!m_captureDeviceInitialised || m_captureRunning) return false;

    ma_result result = ma_device_start(&m_captureDevice);
    if (result != MA_SUCCESS) { return false; }

    m_captureRunning = true;
    return true;
}

void AudioDeviceManager::stopCapture() {
    if (!m_captureRunning) return;

    ma_device_stop(&m_captureDevice);
    m_captureRunning = false;
    m_captureAudioThreadRegistered.store(false, std::memory_order_relaxed);

    m_captureCallbacks.read([](const auto& callbacks) {
        for (auto* callback : callbacks) { callback->releaseResources(); }
    });
}

bool AudioDeviceManager::startDuplex() {
    if (!m_duplexDeviceInitialised || m_duplexRunning) return false;

    ma_result result = ma_device_start(&m_duplexDevice);
    if (result != MA_SUCCESS) { return false; }

    m_duplexRunning = true;
    return true;
}

void AudioDeviceManager::stopDuplex() {
    if (!m_duplexRunning) return;

    ma_device_stop(&m_duplexDevice);
    m_duplexRunning = false;
    m_duplexAudioThreadRegistered.store(false, std::memory_order_relaxed);

    m_duplexCallbacks.read([](const auto& callbacks) {
        for (auto* callback : callbacks) { callback->releaseResources(); }
    });
}

void AudioDeviceManager::addPlaybackCallback(AudioIODeviceCallback* callback) {
    if (!callback) return;

    bool added = false;
    m_playbackCallbacks.update([&](auto& callbacks) {
        if (std::find(callbacks.begin(), callbacks.end(), callback) ==
            callbacks.end()) {
            callbacks.push_back(callback);
            added = true;
        }
    });

    if (added && m_playbackDeviceInitialised) {
        callback->prepareToPlay(m_playbackDevice.sampleRate,
                                m_playbackBufferSize);
    }
}

void AudioDeviceManager::addCaptureCallback(AudioIODeviceCallback* callback) {
    if (!callback) return;

    bool added = false;
    m_captureCallbacks.update([&](auto& callbacks) {
        if (std::find(callbacks.begin(), callbacks.end(), callback) ==
            callbacks.end()) {
            callbacks.push_back(callback);
            added = true;
        }
    });

    if (added && m_captureDeviceInitialised) {
        callback->prepareToPlay(m_captureDevice.sampleRate,
                                m_captureBufferSize);
    }
}

void AudioDeviceManager::addDuplexCallback(AudioIODeviceCallback* callback) {
    if (!callback) return;

    bool added = false;
    m_duplexCallbacks.update([&](auto& callbacks) {
        if (std::find(callbacks.begin(), callbacks.end(), callback) ==
            callbacks.end()) {
            callbacks.push_back(callback);
            added = true;
        }
    });

    if (added && m_duplexDeviceInitialised) {
        callback->prepareToPlay(m_duplexDevice.sampleRate, m_duplexBufferSize);
    }
}

void AudioDeviceManager::removePlaybackCallback(
    AudioIODeviceCallback* callback) {
    AudioIODeviceCallback* removed = nullptr;
    m_playbackCallbacks.update([&](auto& callbacks) {
        auto it = std::find(callbacks.begin(), callbacks.end(), callback);
        if (it != callbacks.end()) {
            removed = *it;
            callbacks.erase(it);
        }
    });

    if (removed) { removed->releaseResources(); }
}

void AudioDeviceManager::removeCaptureCallback(
    AudioIODeviceCallback* callback) {
    AudioIODeviceCallback* removed = nullptr;
    m_captureCallbacks.update([&](auto& callbacks) {
        auto it = std::find(callbacks.begin(), callbacks.end(), callback);
        if (it != callbacks.end()) {
            removed = *it;
            callbacks.erase(it);
        }
    });

    if (removed) { removed->releaseResources(); }
}

void AudioDeviceManager::removeDuplexCallback(AudioIODeviceCallback* callback) {
    AudioIODeviceCallback* removed = nullptr;
    m_duplexCallbacks.update([&](auto& callbacks) {
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
    auto ptr =
        callback
            ? std::make_shared<DeviceNotificationCallback>(std::move(callback))
            : nullptr;
    std::atomic_store_explicit(&m_notificationCallback,
                               std::move(ptr),
                               std::memory_order_release);
}

bool AudioDeviceManager::registerLogCallback(ma_log_callback_proc callback,
                                             void* userData) {
    if (!m_contextInitialised || !callback) { return false; }
    ma_log* log = ma_context_get_log(&m_context);
    if (!log) { return false; }
    return ma_log_register_callback(log,
                                    ma_log_callback_init(callback, userData)) ==
           MA_SUCCESS;
}

bool AudioDeviceManager::unregisterLogCallback(ma_log_callback_proc callback,
                                               void* userData) {
    if (!m_contextInitialised || !callback) { return false; }
    ma_log* log = ma_context_get_log(&m_context);
    if (!log) { return false; }
    return ma_log_unregister_callback(
               log,
               ma_log_callback_init(callback, userData)) == MA_SUCCESS;
}

void AudioDeviceManager::dataCallback(ma_device* pDevice,
                                      void* pOutput,
                                      const void* pInput,
                                      ma_uint32 frameCount) {
    auto* userData = static_cast<DeviceUserData*>(pDevice->pUserData);
    if (!userData || !userData->manager) { return; }

    userData->manager->processCallbacks(userData->role,
                                        pDevice,
                                        static_cast<float*>(pOutput),
                                        static_cast<const float*>(pInput),
                                        frameCount);
}

void AudioDeviceManager::notificationCallback(
    const ma_device_notification* pNotification) {
    auto* userData =
        static_cast<DeviceUserData*>(pNotification->pDevice->pUserData);
    if (!userData || !userData->manager) { return; }

    auto cb =
        std::atomic_load_explicit(&userData->manager->m_notificationCallback,
                                  std::memory_order_acquire);
    if (cb) { (*cb)(pNotification->type); }
}

void AudioDeviceManager::processCallbacks(DeviceRole role,
                                          ma_device* device,
                                          float* output,
                                          const float* input,
                                          ma_uint32 frameCount) {
    switch (role) {
        case DeviceRole::Playback:
            dispatchCallbacks(m_playbackCallbacks,
                              m_playbackAudioThreadRegistered,
                              device,
                              output,
                              input,
                              frameCount);
            break;
        case DeviceRole::Capture:
            dispatchCallbacks(m_captureCallbacks,
                              m_captureAudioThreadRegistered,
                              device,
                              output,
                              input,
                              frameCount);
            break;
        case DeviceRole::Duplex:
            dispatchCallbacks(m_duplexCallbacks,
                              m_duplexAudioThreadRegistered,
                              device,
                              output,
                              input,
                              frameCount);
            break;
    }
}

void AudioDeviceManager::dispatchCallbacks(
    RCU<std::vector<AudioIODeviceCallback*>>& callbacks,
    std::atomic<bool>& audioThreadRegistered,
    ma_device* device,
    float* output,
    const float* input,
    ma_uint32 frameCount) {
    if (!device) { return; }

    // Register audio thread with RCU on first callback (not RT-safe, but only
    // happens once)
    if (!audioThreadRegistered.load(std::memory_order_relaxed)) [[unlikely]] {
        callbacks.register_reader_thread();
        audioThreadRegistered.store(true, std::memory_order_relaxed);
    }

    ma_uint32 outputChannels = device->playback.channels;
    ma_uint32 inputChannels = device->capture.channels;

    if (outputChannels == 0 && inputChannels == 0) { return; }

    if (output && outputChannels > 0) {
        std::memset(output, 0, frameCount * outputChannels * sizeof(float));
    }

    float* safeOutput = outputChannels > 0 ? output : nullptr;
    const float* safeInput = inputChannels > 0 ? input : nullptr;

    callbacks.read([&](const auto& list) {
        for (auto* callback : list) {
            callback->process(safeOutput,
                              safeInput,
                              frameCount,
                              inputChannels,
                              outputChannels);
        }
    });
}

}  // namespace thl
