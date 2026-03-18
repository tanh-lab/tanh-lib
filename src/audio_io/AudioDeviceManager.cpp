#include <tanh/audio_io/AudioDeviceManager.h>
#include "miniaudio.h"
#include "tanh/core/Logger.h"
#include <algorithm>
#include <cstring>
#include <vector>

#if defined(THL_PLATFORM_MACOS) || defined(THL_PLATFORM_IOS)
#include <TargetConditionals.h>
#endif

namespace thl {

struct AudioDeviceManager::DeviceUserData {
    AudioDeviceManager* manager = nullptr;
    AudioDeviceManager::DeviceRole role = AudioDeviceManager::DeviceRole::Playback;
};

struct AudioDeviceManager::Impl {
    ma_context context;
    ma_device playbackDevice;
    ma_device captureDevice;
    ma_device duplexDevice;

    bool contextInitialised = false;
    bool playbackDeviceInitialised = false;
    bool captureDeviceInitialised = false;
    bool duplexDeviceInitialised = false;

    // Per-callback period size (burst on Android, period on iOS/macOS).
    uint32_t playbackPeriodSize = 512;
    uint32_t capturePeriodSize = 512;
    uint32_t duplexPeriodSize = 512;

    // Number of periods (Android: bufferSize / burst, iOS: 1).
    uint32_t playbackPeriods = 1;
    uint32_t capturePeriods = 1;
    uint32_t duplexPeriods = 1;

    DeviceUserData playbackUserData;
    DeviceUserData captureUserData;
    DeviceUserData duplexUserData;

    LogCallback logCallback;
};

namespace {

DeviceNotificationType toNotificationType(ma_device_notification_type type) {
    switch (type) {
        case ma_device_notification_type_started: return DeviceNotificationType::Started;
        case ma_device_notification_type_stopped: return DeviceNotificationType::Stopped;
        case ma_device_notification_type_rerouted: return DeviceNotificationType::Rerouted;
        case ma_device_notification_type_interruption_began:
            return DeviceNotificationType::InterruptionBegan;
        case ma_device_notification_type_interruption_ended:
            return DeviceNotificationType::InterruptionEnded;
        case ma_device_notification_type_unlocked: return DeviceNotificationType::Unlocked;
    }
    return DeviceNotificationType::Stopped;
}

DeviceType toDeviceType(ma_device_type type) {
    switch (type) {
        case ma_device_type_playback: return DeviceType::Playback;
        case ma_device_type_capture: return DeviceType::Capture;
        case ma_device_type_duplex: return DeviceType::Duplex;
        default: return DeviceType::Playback;
    }
}

ma_device_type toMaDeviceType(DeviceType type) {
    switch (type) {
        case DeviceType::Playback: return ma_device_type_playback;
        case DeviceType::Capture: return ma_device_type_capture;
        case DeviceType::Duplex: return ma_device_type_duplex;
    }
    return ma_device_type_playback;
}

struct EnumUserData {
    std::vector<AudioDeviceInfo>* devices;
    ma_device_type targetType;
};

ma_bool32 enumCallback(ma_context* /*pContext*/,
                       ma_device_type deviceType,
                       const ma_device_info* pInfo,
                       void* pUserData) {
    auto* userData = static_cast<EnumUserData*>(pUserData);
    if (deviceType != userData->targetType) return MA_TRUE;

    static_assert(sizeof(ma_device_id) <= AudioDeviceInfo::kDeviceIdStorageSize,
                  "ma_device_id too large for storage");

    AudioDeviceInfo info;
    info.name = pInfo->name;
    info.deviceType = toDeviceType(deviceType);
    std::memcpy(info.deviceIdStoragePtr(), &pInfo->id, sizeof(ma_device_id));
    userData->devices->push_back(std::move(info));
    return MA_TRUE;
}

}  // namespace

AudioDeviceManager::AudioDeviceManager() : m_impl(std::make_unique<Impl>()) {
    m_impl->playbackUserData.manager = this;
    m_impl->playbackUserData.role = DeviceRole::Playback;
    m_impl->captureUserData.manager = this;
    m_impl->captureUserData.role = DeviceRole::Capture;
    m_impl->duplexUserData.manager = this;
    m_impl->duplexUserData.role = DeviceRole::Duplex;

    ma_context_config ctxConfig = ma_context_config_init();

#if defined(THL_PLATFORM_IOS)
    ctxConfig.coreaudio.sessionCategory = ma_ios_session_category_play_and_record;
    ctxConfig.coreaudio.sessionCategoryOptions = ma_ios_session_category_option_default_to_speaker |
                                                 ma_ios_session_category_option_allow_bluetooth;
#endif

    ma_result result = ma_context_init(nullptr, 0, &ctxConfig, &m_impl->context);
    m_impl->contextInitialised = (result == MA_SUCCESS);
}

AudioDeviceManager::~AudioDeviceManager() {
    shutdown();
    if (m_impl->contextInitialised) {
        ma_context_uninit(&m_impl->context);
        m_impl->contextInitialised = false;
    }
}

bool AudioDeviceManager::isContextInitialised() const {
    return m_impl->contextInitialised;
}

void AudioDeviceManager::populateSampleRates(std::vector<AudioDeviceInfo>& devices) const {
    for (auto& info : devices) {
        ma_device_info deviceInfo;
        ma_device_id deviceId{};
        std::memcpy(&deviceId, info.deviceIDPtr(), sizeof(deviceId));
        ma_result result = ma_context_get_device_info(const_cast<ma_context*>(&m_impl->context),
                                                      toMaDeviceType(info.deviceType),
                                                      &deviceId,
                                                      &deviceInfo);

        std::vector<uint32_t> rates;
        if (result == MA_SUCCESS) {
            for (ma_uint32 i = 0; i < deviceInfo.nativeDataFormatCount; i++) {
                uint32_t rate = deviceInfo.nativeDataFormats[i].sampleRate;
                if (rate > 0) {
                    if (std::find(rates.begin(), rates.end(), rate) == rates.end()) {
                        rates.push_back(rate);
                    }
                }
            }
        }

        if (rates.empty()) {
            ma_log* log = ma_context_get_log(const_cast<ma_context*>(&m_impl->context));
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

std::vector<AudioDeviceInfo> AudioDeviceManager::enumerateDevices(DeviceType type) const {
    if (!m_impl->contextInitialised) return {};

    std::vector<AudioDeviceInfo> devices;
    EnumUserData userData{&devices, toMaDeviceType(type)};
    ma_context_enumerate_devices(const_cast<ma_context*>(&m_impl->context),
                                 enumCallback,
                                 &userData);
    populateSampleRates(devices);
    return devices;
}

std::vector<AudioDeviceInfo> AudioDeviceManager::enumerateInputDevices() const {
    return enumerateDevices(DeviceType::Capture);
}

std::vector<AudioDeviceInfo> AudioDeviceManager::enumerateOutputDevices() const {
    return enumerateDevices(DeviceType::Playback);
}

bool AudioDeviceManager::tryInitialiseDevice(DeviceRole role,
                                             const AudioDeviceInfo* inputDevice,
                                             const AudioDeviceInfo* outputDevice,
                                             uint32_t sampleRate,
                                             uint32_t bufferSizeInFrames,
                                             uint32_t numInputChannels,
                                             uint32_t numOutputChannels) {
    ma_device* device = nullptr;
    DeviceUserData* userData = nullptr;
    ma_device_type deviceType = ma_device_type_playback;
    const char* label = nullptr;

    switch (role) {
        case DeviceRole::Playback:
            if (!outputDevice) { return false; }
            device = &m_impl->playbackDevice;
            userData = &m_impl->playbackUserData;
            deviceType = ma_device_type_playback;
            label = "playback";
            break;
        case DeviceRole::Capture:
            if (!inputDevice) { return false; }
            device = &m_impl->captureDevice;
            userData = &m_impl->captureUserData;
            deviceType = ma_device_type_capture;
            label = "capture";
            break;
        case DeviceRole::Duplex:
            if (!inputDevice || !outputDevice) { return false; }
            device = &m_impl->duplexDevice;
            userData = &m_impl->duplexUserData;
            deviceType = ma_device_type_duplex;
            label = "duplex";
            break;
    }

    ma_device_config devConfig = ma_device_config_init(deviceType);
    devConfig.sampleRate = sampleRate;
    devConfig.dataCallback =
        reinterpret_cast<ma_device_data_proc>(&AudioDeviceManager::dataCallback);
    devConfig.notificationCallback =
        reinterpret_cast<ma_device_notification_proc>(&AudioDeviceManager::notificationCallback);
    devConfig.pUserData = userData;

    ma_device_id outputDeviceId{};
    ma_device_id inputDeviceId{};
    if (role == DeviceRole::Playback || role == DeviceRole::Duplex) {
        std::memcpy(&outputDeviceId, outputDevice->deviceIDPtr(), sizeof(outputDeviceId));
        devConfig.playback.pDeviceID = &outputDeviceId;
        devConfig.playback.format = ma_format_f32;
        devConfig.playback.channels = numOutputChannels;
    }

    if (role == DeviceRole::Capture || role == DeviceRole::Duplex) {
        std::memcpy(&inputDeviceId, inputDevice->deviceIDPtr(), sizeof(inputDeviceId));
        devConfig.capture.pDeviceID = &inputDeviceId;
        devConfig.capture.format = ma_format_f32;
        devConfig.capture.channels = numInputChannels;
    }

    uint32_t configuredPeriods = 1;

#if defined(THL_PLATFORM_ANDROID)
    // Android: probe for the hardware burst size, then configure
    // the period to one burst and the period count to the requested
    // multiple so that total buffer = burst × periods.
    {
        ma_device_config probeConfig = devConfig;
        probeConfig.periodSizeInFrames = 0;
        probeConfig.noFixedSizedCallback = MA_TRUE;
        probeConfig.dataCallback = nullptr;
        probeConfig.notificationCallback = nullptr;
        probeConfig.pUserData = nullptr;

        ma_device probeDevice;
        uint32_t burstSize = 0;
        if (ma_device_init(&m_impl->context, &probeConfig, &probeDevice) == MA_SUCCESS) {
            burstSize = probeDevice.playback.internalPeriodSizeInFrames;
            if (burstSize == 0) burstSize = probeDevice.capture.internalPeriodSizeInFrames;
            ma_device_uninit(&probeDevice);
        }

        if (burstSize > 0) {
            // Default to 1 burst when no buffer size is specified.
            if (bufferSizeInFrames == 0) bufferSizeInFrames = burstSize;

            configuredPeriods = (bufferSizeInFrames + burstSize - 1) / burstSize;
            if (configuredPeriods == 0) configuredPeriods = 1;

            if (bufferSizeInFrames % burstSize != 0) {
                uint32_t aligned = burstSize * configuredPeriods;
                thl::Logger::logf(thl::Logger::LogLevel::Warning,
                                   "thl.audio_io.audio_device_manager",
                                   "Requested buffer size %u is not a multiple of burst "
                                   "size %u, rounding up to %u",
                                   bufferSizeInFrames, burstSize, aligned);
                bufferSizeInFrames = aligned;
            }

            devConfig.periodSizeInFrames = burstSize;
            devConfig.periods = configuredPeriods;

            thl::Logger::logf(thl::Logger::LogLevel::Debug,
                               "thl.audio_io.audio_device_manager",
                               "Android: burst size %u, period count %u, total buffer %u frames",
                               burstSize, configuredPeriods, bufferSizeInFrames);
        } else {
            // Fallback when burst size could not be probed.
            devConfig.periodSizeInFrames =
                bufferSizeInFrames > 0 ? bufferSizeInFrames : 256;
        }
    }
#elif defined(THL_PLATFORM_IOS) || defined(THL_PLATFORM_MACOS)
    // Apple: probe the device to discover the actual hardware period size,
    // then keep periods = 1 and use whatever CoreAudio actually delivers.
    {
        ma_device_config probeConfig = devConfig;
        probeConfig.periodSizeInFrames = bufferSizeInFrames;
        probeConfig.periods = 1;
        probeConfig.noFixedSizedCallback = MA_TRUE;
        probeConfig.dataCallback = nullptr;
        probeConfig.notificationCallback = nullptr;
        probeConfig.pUserData = nullptr;

        ma_device probeDevice;
        uint32_t hwPeriod = 0;
        if (ma_device_init(&m_impl->context, &probeConfig, &probeDevice) == MA_SUCCESS) {
            hwPeriod = probeDevice.playback.internalPeriodSizeInFrames;
            if (hwPeriod == 0) hwPeriod = probeDevice.capture.internalPeriodSizeInFrames;
            ma_device_uninit(&probeDevice);
        }

        if (hwPeriod > 0 && hwPeriod != bufferSizeInFrames) {
            thl::Logger::logf(thl::Logger::LogLevel::Warning,
                               "thl.audio_io.audio_device_manager",
                               "CoreAudio returned period size %u instead of requested %u, "
                               "adjusting to hardware value",
                               hwPeriod, bufferSizeInFrames);
            bufferSizeInFrames = hwPeriod;
        }

        devConfig.periodSizeInFrames = bufferSizeInFrames;
        devConfig.periods = 1;
    }
#else
    devConfig.periodSizeInFrames = bufferSizeInFrames;
#endif

    if (ma_device_init(&m_impl->context, &devConfig, device) != MA_SUCCESS) { return false; }

    // Read the hardware burst size (what AAudio / CoreAudio actually delivers).
    uint32_t hw = device->playback.internalPeriodSizeInFrames;
    if (hw == 0) hw = device->capture.internalPeriodSizeInFrames;
    uint32_t resolvedSize = (hw > 0) ? hw : bufferSizeInFrames;

    thl::Logger::logf(thl::Logger::LogLevel::Debug,
                       "thl.audio_io.audio_device_manager",
                       "Initialised %s device '%s' with sample rate %u Hz, buffer size %u frames, "
                       "%u output channels, %u input channels",
                       label,
                       (role == DeviceRole::Playback || role == DeviceRole::Duplex) ? outputDevice->name.c_str()
                                                                                    : inputDevice->name.c_str(),
                       sampleRate,
                       resolvedSize,
                       numOutputChannels,
                       numInputChannels);
    thl::Logger::logf(thl::Logger::LogLevel::Debug,
                       "thl.audio_io.audio_device_manager",
                       "Requested buffer size was %u frames, got buffer size of %u frames",
                       bufferSizeInFrames,
                       resolvedSize);

    switch (role) {
        case DeviceRole::Playback:
            m_impl->playbackDeviceInitialised = true;
            m_numOutputChannels = device->playback.channels;
            m_sampleRate = device->sampleRate;
            m_impl->playbackPeriodSize = resolvedSize;
            m_impl->playbackPeriods = configuredPeriods;
            break;
        case DeviceRole::Capture:
            m_impl->captureDeviceInitialised = true;
            m_numInputChannels = device->capture.channels;
            if (!m_impl->playbackDeviceInitialised) { m_sampleRate = device->sampleRate; }
            m_impl->capturePeriodSize = resolvedSize;
            m_impl->capturePeriods = configuredPeriods;
            break;
        case DeviceRole::Duplex:
            m_impl->duplexDeviceInitialised = true;
            if (!m_impl->playbackDeviceInitialised && !m_impl->captureDeviceInitialised) {
                m_sampleRate = device->sampleRate;
            }
            m_impl->duplexPeriodSize = resolvedSize;
            m_impl->duplexPeriods = configuredPeriods;
            break;
    }

    return true;
}

bool AudioDeviceManager::initialise(const AudioDeviceInfo* inputDevice,
                                    const AudioDeviceInfo* outputDevice,
                                    uint32_t sampleRate,
                                    uint32_t bufferSizeInFrames,
                                    uint32_t numInputChannels,
                                    uint32_t numOutputChannels) {
    if (!m_impl->contextInitialised) return false;

    thl::Logger::logf(thl::Logger::LogLevel::Info,
                      "thl.audio_io.audio_device_manager",
                      "Initialise from C++");

    shutdown();

    m_sampleRate = sampleRate;
    m_bufferSize = bufferSizeInFrames;
    m_numInputChannels = inputDevice ? numInputChannels : 0;
    m_numOutputChannels = outputDevice ? numOutputChannels : 0;
    m_impl->playbackPeriodSize = bufferSizeInFrames;
    m_impl->capturePeriodSize = bufferSizeInFrames;
    m_impl->duplexPeriodSize = bufferSizeInFrames;
    m_impl->playbackPeriods = 1;
    m_impl->capturePeriods = 1;
    m_impl->duplexPeriods = 1;

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

    if (m_impl->playbackDeviceInitialised) {
        m_bufferSize = m_impl->playbackPeriodSize * m_impl->playbackPeriods;
    } else if (m_impl->duplexDeviceInitialised) {
        m_bufferSize = m_impl->duplexPeriodSize * m_impl->duplexPeriods;
    } else if (m_impl->captureDeviceInitialised) {
        m_bufferSize = m_impl->capturePeriodSize * m_impl->capturePeriods;
    }

    auto prepareCallbacks =
        [](RCU<std::vector<AudioIODeviceCallback*>>& list, ma_device& device, uint32_t bufferSize) {
            list.read([&](const auto& callbacks) {
                for (auto* callback : callbacks) {
                    callback->prepareToPlay(device.sampleRate, bufferSize);
                }
            });
        };

    if (m_impl->playbackDeviceInitialised) {
        prepareCallbacks(m_playbackCallbacks, m_impl->playbackDevice, m_impl->playbackPeriodSize);
    }
    if (m_impl->captureDeviceInitialised) {
        prepareCallbacks(m_captureCallbacks, m_impl->captureDevice, m_impl->capturePeriodSize);
    }
    if (m_impl->duplexDeviceInitialised) {
        prepareCallbacks(m_duplexCallbacks, m_impl->duplexDevice, m_impl->duplexPeriodSize);
    }

    return true;
}

void AudioDeviceManager::shutdown() {
    stopPlayback();
    stopCapture();
    stopDuplex();

    if (m_impl->playbackDeviceInitialised) {
        ma_device_uninit(&m_impl->playbackDevice);
        m_impl->playbackDeviceInitialised = false;
    }
    if (m_impl->captureDeviceInitialised) {
        ma_device_uninit(&m_impl->captureDevice);
        m_impl->captureDeviceInitialised = false;
    }
    if (m_impl->duplexDeviceInitialised) {
        ma_device_uninit(&m_impl->duplexDevice);
        m_impl->duplexDeviceInitialised = false;
    }
}

bool AudioDeviceManager::startPlayback() {
    if (!m_impl->playbackDeviceInitialised || m_playbackRunning) return false;

    ma_result result = ma_device_start(&m_impl->playbackDevice);
    if (result != MA_SUCCESS) { return false; }

    m_playbackRunning = true;
    return true;
}

void AudioDeviceManager::stopPlayback() {
    if (!m_playbackRunning) return;

    ma_device_stop(&m_impl->playbackDevice);
    m_playbackRunning = false;
    m_playbackAudioThreadRegistered.store(false, std::memory_order_relaxed);

    m_playbackCallbacks.read([](const auto& callbacks) {
        for (auto* callback : callbacks) { callback->releaseResources(); }
    });
}

bool AudioDeviceManager::startCapture() {
    if (!m_impl->captureDeviceInitialised || m_captureRunning) return false;

    ma_result result = ma_device_start(&m_impl->captureDevice);
    if (result != MA_SUCCESS) { return false; }

    m_captureRunning = true;
    return true;
}

void AudioDeviceManager::stopCapture() {
    if (!m_captureRunning) return;

    ma_device_stop(&m_impl->captureDevice);
    m_captureRunning = false;
    m_captureAudioThreadRegistered.store(false, std::memory_order_relaxed);

    m_captureCallbacks.read([](const auto& callbacks) {
        for (auto* callback : callbacks) { callback->releaseResources(); }
    });
}

bool AudioDeviceManager::startDuplex() {
    if (!m_impl->duplexDeviceInitialised || m_duplexRunning) return false;

    ma_result result = ma_device_start(&m_impl->duplexDevice);
    if (result != MA_SUCCESS) { return false; }

    m_duplexRunning = true;
    return true;
}

void AudioDeviceManager::stopDuplex() {
    if (!m_duplexRunning) return;

    ma_device_stop(&m_impl->duplexDevice);
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
        if (std::find(callbacks.begin(), callbacks.end(), callback) == callbacks.end()) {
            callbacks.push_back(callback);
            added = true;
        }
    });

    if (added && m_impl->playbackDeviceInitialised) {
        callback->prepareToPlay(m_impl->playbackDevice.sampleRate, m_impl->playbackPeriodSize);
    }
}

void AudioDeviceManager::addCaptureCallback(AudioIODeviceCallback* callback) {
    if (!callback) return;

    bool added = false;
    m_captureCallbacks.update([&](auto& callbacks) {
        if (std::find(callbacks.begin(), callbacks.end(), callback) == callbacks.end()) {
            callbacks.push_back(callback);
            added = true;
        }
    });

    if (added && m_impl->captureDeviceInitialised) {
        callback->prepareToPlay(m_impl->captureDevice.sampleRate, m_impl->capturePeriodSize);
    }
}

void AudioDeviceManager::addDuplexCallback(AudioIODeviceCallback* callback) {
    if (!callback) return;

    bool added = false;
    m_duplexCallbacks.update([&](auto& callbacks) {
        if (std::find(callbacks.begin(), callbacks.end(), callback) == callbacks.end()) {
            callbacks.push_back(callback);
            added = true;
        }
    });

    if (added && m_impl->duplexDeviceInitialised) {
        callback->prepareToPlay(m_impl->duplexDevice.sampleRate, m_impl->duplexPeriodSize);
    }
}

void AudioDeviceManager::removePlaybackCallback(AudioIODeviceCallback* callback) {
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

void AudioDeviceManager::removeCaptureCallback(AudioIODeviceCallback* callback) {
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

void AudioDeviceManager::setDeviceNotificationCallback(DeviceNotificationCallback callback) {
    auto ptr =
        callback ? std::make_shared<DeviceNotificationCallback>(std::move(callback)) : nullptr;
    std::atomic_store_explicit(&m_notificationCallback, std::move(ptr), std::memory_order_release);
}

void AudioDeviceManager::setLogCallback(LogCallback callback) {
    m_impl->logCallback = std::move(callback);

    if (!m_impl->contextInitialised) return;

    ma_log* log = ma_context_get_log(&m_impl->context);
    if (!log) return;

    if (m_impl->logCallback) {
        ma_log_register_callback(log, ma_log_callback_init(staticLogCallback, m_impl.get()));
    } else {
        ma_log_unregister_callback(log, ma_log_callback_init(staticLogCallback, m_impl.get()));
    }
}

uint32_t AudioDeviceManager::getSampleRate() const {
    if (m_impl->playbackDeviceInitialised) { return m_impl->playbackDevice.sampleRate; }
    if (m_impl->duplexDeviceInitialised) { return m_impl->duplexDevice.sampleRate; }
    if (m_impl->captureDeviceInitialised) { return m_impl->captureDevice.sampleRate; }
    return m_sampleRate;
}

uint32_t AudioDeviceManager::getBufferSize() const {
    return m_bufferSize;
}

uint32_t AudioDeviceManager::getPeriodSize() const {
    if (m_impl->playbackDeviceInitialised) { return m_impl->playbackPeriodSize; }
    if (m_impl->duplexDeviceInitialised) { return m_impl->duplexPeriodSize; }
    if (m_impl->captureDeviceInitialised) { return m_impl->capturePeriodSize; }
    return m_bufferSize;
}

uint32_t AudioDeviceManager::getPeriodCount() const {
    if (m_impl->playbackDeviceInitialised) { return m_impl->playbackPeriods; }
    if (m_impl->duplexDeviceInitialised) { return m_impl->duplexPeriods; }
    if (m_impl->captureDeviceInitialised) { return m_impl->capturePeriods; }
    return 1;
}

uint32_t AudioDeviceManager::getNumInputChannels() const {
    if (m_impl->captureDeviceInitialised) { return m_impl->captureDevice.capture.channels; }
    if (m_impl->duplexDeviceInitialised) { return m_impl->duplexDevice.capture.channels; }
    return m_numInputChannels;
}

uint32_t AudioDeviceManager::getNumOutputChannels() const {
    if (m_impl->playbackDeviceInitialised) { return m_impl->playbackDevice.playback.channels; }
    if (m_impl->duplexDeviceInitialised) { return m_impl->duplexDevice.playback.channels; }
    return m_numOutputChannels;
}

void AudioDeviceManager::processCallbacks(DeviceRole role,
                                          void* devicePtr,
                                          float* output,
                                          const float* input,
                                          uint32_t frameCount) {
    auto* device = static_cast<ma_device*>(devicePtr);
    RCU<std::vector<AudioIODeviceCallback*>>* callbacks = nullptr;
    std::atomic<bool>* audioThreadRegistered = nullptr;
    uint32_t preparedBufferSize = 0;

    switch (role) {
        case DeviceRole::Playback:
            callbacks = &m_playbackCallbacks;
            audioThreadRegistered = &m_playbackAudioThreadRegistered;
            preparedBufferSize = m_impl->playbackPeriodSize;
            break;
        case DeviceRole::Capture:
            callbacks = &m_captureCallbacks;
            audioThreadRegistered = &m_captureAudioThreadRegistered;
            preparedBufferSize = m_impl->capturePeriodSize;
            break;
        case DeviceRole::Duplex:
            callbacks = &m_duplexCallbacks;
            audioThreadRegistered = &m_duplexAudioThreadRegistered;
            preparedBufferSize = m_impl->duplexPeriodSize;
            break;
    }

    if (!device || !callbacks || !audioThreadRegistered) { return; }

    if (!audioThreadRegistered->load(std::memory_order_relaxed)) [[unlikely]] {
        callbacks->register_reader_thread();
        audioThreadRegistered->store(true, std::memory_order_relaxed);
    }

    uint32_t outputChannels = device->playback.channels;
    uint32_t inputChannels = device->capture.channels;

    if (outputChannels == 0 && inputChannels == 0) { return; }

    if (output && outputChannels > 0) {
        std::memset(output, 0, frameCount * outputChannels * sizeof(float));
    }

    float* safeOutput = outputChannels > 0 ? output : nullptr;
    const float* safeInput = inputChannels > 0 ? input : nullptr;

    // Process in chunks no larger than the prepared buffer size to avoid
    // overflowing internal DSP buffers.
    uint32_t framesRemaining = frameCount;
    uint32_t offset = 0;

    while (framesRemaining > 0) {
        uint32_t chunkSize = (preparedBufferSize > 0)
                                 ? std::min(framesRemaining, preparedBufferSize)
                                 : framesRemaining;

        float* chunkOutput =
            safeOutput ? safeOutput + offset * outputChannels : nullptr;
        const float* chunkInput =
            safeInput ? safeInput + offset * inputChannels : nullptr;

        callbacks->read([&](const auto& list) {
            for (auto* callback : list) {
                callback->process(
                    chunkOutput, chunkInput, chunkSize, inputChannels, outputChannels);
            }
        });

        offset += chunkSize;
        framesRemaining -= chunkSize;
    }
}

void AudioDeviceManager::dataCallback(void* pDeviceVoid,
                                      void* pOutput,
                                      const void* pInput,
                                      uint32_t frameCount) {
    auto* pDevice = static_cast<ma_device*>(pDeviceVoid);
    auto* userData = static_cast<DeviceUserData*>(pDevice->pUserData);
    if (!userData || !userData->manager) { return; }

    userData->manager->processCallbacks(userData->role,
                                        pDevice,
                                        static_cast<float*>(pOutput),
                                        static_cast<const float*>(pInput),
                                        frameCount);
}

void AudioDeviceManager::notificationCallback(const void* pNotificationVoid) {
    auto* pNotification = static_cast<const ma_device_notification*>(pNotificationVoid);
    auto* userData = static_cast<DeviceUserData*>(pNotification->pDevice->pUserData);
    if (!userData || !userData->manager) { return; }

    auto cb = std::atomic_load_explicit(&userData->manager->m_notificationCallback,
                                        std::memory_order_acquire);
    if (cb) { (*cb)(toNotificationType(pNotification->type)); }
}

void AudioDeviceManager::staticLogCallback(void* pUserData, uint32_t level, const char* pMessage) {
    auto* impl = static_cast<Impl*>(pUserData);
    if (!impl || !impl->logCallback) { return; }

    // Miniaudio levels: 1=error, 2=warning, 3=info, 4=debug.
    Logger::LogLevel normalised;
    switch (level) {
        case 1: normalised = Logger::LogLevel::Error; break;
        case 2: normalised = Logger::LogLevel::Warning; break;
        case 3: normalised = Logger::LogLevel::Info; break;
        case 4: normalised = Logger::LogLevel::Debug; break;
        default: normalised = Logger::LogLevel::Info; break;
    }
    impl->logCallback(normalised, pMessage);
}

}  // namespace thl
