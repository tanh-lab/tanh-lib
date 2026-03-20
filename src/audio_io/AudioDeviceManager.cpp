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

    // Hardware burst size (Android: AAudio framesPerBurst, Apple: same as periodSize).
    uint32_t burstSize = 0;

    // On iOS, the actual Audio Unit render callback frame count can differ
    // from what AVAudioSession / MaximumFramesPerSlice report.
    // These atomics are written once from the first callback and read by
    // getPeriodSize() / getBurstSize().
    std::atomic<uint32_t> playbackActualPeriodSize{0};
    std::atomic<uint32_t> captureActualPeriodSize{0};
    std::atomic<uint32_t> duplexActualPeriodSize{0};

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

#if defined(THL_PLATFORM_ANDROID)
static uint32_t probeAAudioBurstSize(ma_context& context,
                                     const ma_device_config& devConfig,
                                     const char* label) {
    ma_device_config probeConfig = devConfig;
    probeConfig.periodSizeInFrames = 0;
    probeConfig.noFixedSizedCallback = MA_TRUE;
    // Probe without setting buffer capacity to discover the natural burst.
    probeConfig.aaudio.allowSetBufferCapacity = MA_FALSE;
    probeConfig.dataCallback = nullptr;
    probeConfig.notificationCallback = nullptr;
    probeConfig.pUserData = nullptr;

    uint32_t burstSize = 0;
    ma_device probeDevice;
    if (ma_device_init(&context, &probeConfig, &probeDevice) == MA_SUCCESS) {
        auto* pStream = probeDevice.aaudio.pStreamPlayback
                            ? probeDevice.aaudio.pStreamPlayback
                            : probeDevice.aaudio.pStreamCapture;

        // Read burst and capacity directly from the AAudio stream.
        int32_t probedBurst = 0;
        int32_t probedCapacity = 0;
        if (pStream) {
            if (context.aaudio.AAudioStream_getFramesPerBurst) {
                probedBurst = reinterpret_cast<int32_t (*)(void*)>(
                    context.aaudio.AAudioStream_getFramesPerBurst)(pStream);
            }
            if (context.aaudio.AAudioStream_getBufferCapacityInFrames) {
                probedCapacity = reinterpret_cast<int32_t (*)(void*)>(
                    context.aaudio.AAudioStream_getBufferCapacityInFrames)(pStream);
            }
        }

        // Fallback to miniaudio's internalPeriodSizeInFrames if AAudio API unavailable.
        uint32_t maPeriod = probeDevice.playback.internalPeriodSizeInFrames;
        if (maPeriod == 0) maPeriod = probeDevice.capture.internalPeriodSizeInFrames;

        if (probedBurst > 0) {
            burstSize = static_cast<uint32_t>(probedBurst);
        } else if (maPeriod > 0) {
            burstSize = maPeriod;
        }

        thl::Logger::logf(thl::Logger::LogLevel::Debug,
                           "thl.audio_io.audio_device_manager",
                           "Android %s probe: AAudio burst=%d, capacity=%d, "
                           "ma internalPeriodSizeInFrames=%u → burstSize=%u",
                           label, probedBurst, probedCapacity, maPeriod, burstSize);

        ma_device_uninit(&probeDevice);
    }

    return burstSize;
}
#endif

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
    // Variable-sized callbacks: AAudio or Core Audio delivers burst-sized chunks directly
    // to our data callback without an intermediary buffer.
    // Note: unlike Oboe, miniaudio does NOT dynamically tune bufferSizeInFrames at runtime.
    devConfig.noFixedSizedCallback = MA_TRUE;
    devConfig.dataCallback =
        reinterpret_cast<ma_device_data_proc>(&AudioDeviceManager::dataCallback);
    devConfig.notificationCallback =
        reinterpret_cast<ma_device_notification_proc>(&AudioDeviceManager::notificationCallback);
    devConfig.pUserData = userData;
#if defined(THL_PLATFORM_ANDROID)
    // Let miniaudio call setBufferCapacityInFrames(periodSize × periods) on the
    // AAudio stream so we control the ring buffer size and thus latency.
    devConfig.aaudio.allowSetBufferCapacity = MA_TRUE;
#endif

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
    uint32_t burstSize = 0;

#if defined(THL_PLATFORM_ANDROID)
    // Probe for the hardware burst size and derive periodSize / periods.
    //
    // Buffer sizes are expected to be power-of-two multiples of burst (1×,2×,4×,8×,…).
    //   1× burst → periodSize = burst,      periods = 1   (lowest latency)
    //   2× burst → periodSize = burst,      periods = 2   (more write-ahead)
    //   4×+      → periodSize = burst×(m/2), periods = 2   (bigger callback = more DSP headroom)
    {
        burstSize = probeAAudioBurstSize(m_impl->context, devConfig, label);

        if (burstSize > 0) {
            // Default to 4× burst when no buffer size requested.
            if (bufferSizeInFrames == 0) bufferSizeInFrames = burstSize * 4;

            // Compute the multiplier as the nearest power-of-two multiple of burst.
            uint32_t m = bufferSizeInFrames / burstSize;
            if (m == 0) m = 1;
            // Round up to next power of two.
            uint32_t pot = 1;
            while (pot < m) pot <<= 1;
            m = pot;
            bufferSizeInFrames = burstSize * m;

            uint32_t periodSize;
            if (m <= 2) {
                periodSize = burstSize;
                configuredPeriods = m;
            } else {
                periodSize = burstSize * (m / 2);
                configuredPeriods = 2;
            }

            devConfig.periodSizeInFrames = periodSize;
            devConfig.periods = configuredPeriods;
        } else {
            devConfig.periodSizeInFrames =
                bufferSizeInFrames > 0 ? bufferSizeInFrames : 256;
        }

        thl::Logger::logf(thl::Logger::LogLevel::Debug,
                           "thl.audio_io.audio_device_manager",
                           "Android %s config: periodSize=%u, periods=%u, total=%u frames",
                           label, devConfig.periodSizeInFrames, devConfig.periods,
                           devConfig.periodSizeInFrames * devConfig.periods);
    }
#elif defined(THL_PLATFORM_IOS) || defined(THL_PLATFORM_MACOS)
    // Apple: probe for the actual hardware period via internalPeriodSizeInFrames.
    // NOTE: On iOS, internalPeriodSizeInFrames reflects kAudioUnitProperty_MaximumFramesPerSlice
    // which is an *upper bound*. The actual Audio Unit render callback may deliver fewer frames
    // (e.g. 512 when MaxFramesPerSlice is 2048). The true period size is resolved from the
    // first callback in processCallbacks().
    {
        ma_device_config probeConfig = devConfig;
        probeConfig.periodSizeInFrames = bufferSizeInFrames;
        probeConfig.periods = 1;
        probeConfig.noFixedSizedCallback = MA_TRUE;
        probeConfig.dataCallback = nullptr;
        probeConfig.notificationCallback = nullptr;
        probeConfig.pUserData = nullptr;

        ma_device probeDevice;
        uint32_t hwPeriodSize = 0;
        uint32_t hwPeriods = 0;
        if (ma_device_init(&m_impl->context, &probeConfig, &probeDevice) == MA_SUCCESS) {
            hwPeriodSize = probeDevice.playback.internalPeriodSizeInFrames;
            hwPeriods = probeDevice.playback.internalPeriods;
            if (hwPeriodSize == 0) hwPeriodSize = probeDevice.capture.internalPeriodSizeInFrames;
            if (hwPeriods == 0) hwPeriods = probeDevice.capture.internalPeriods;
            thl::Logger::logf(thl::Logger::LogLevel::Debug,
                               "thl.audio_io.audio_device_manager",
                               "Apple %s probe: internalPeriodSizeInFrames=%u (requested %u)",
                               label, hwPeriodSize, bufferSizeInFrames);

            ma_device_uninit(&probeDevice);
        }

        if (hwPeriodSize > 0 && hwPeriodSize != bufferSizeInFrames) {
            thl::Logger::logf(thl::Logger::LogLevel::Warning,
                               "thl.audio_io.audio_device_manager",
                               "CoreAudio returned period %u instead of requested %u, adjusting",
                               hwPeriodSize, bufferSizeInFrames);
            bufferSizeInFrames = hwPeriodSize;
        }

        if (hwPeriods > 0 && hwPeriods != 1 && role != DeviceRole::Duplex) {
            thl::Logger::logf(thl::Logger::LogLevel::Warning,
                               "thl.audio_io.audio_device_manager",
                               "Miniaudio with CoreAudio backend returned periods %u instead of expected 1, adjusting",
                               hwPeriods);
        }

        burstSize = bufferSizeInFrames;
        devConfig.periodSizeInFrames = bufferSizeInFrames;
        configuredPeriods = (hwPeriods > 0) ? hwPeriods : 1;
        devConfig.periods = hwPeriods;
    }
#else
    devConfig.periodSizeInFrames = bufferSizeInFrames;
#endif

    if (ma_device_init(&m_impl->context, &devConfig, device) != MA_SUCCESS) { return false; }

    // Resolve the actual period size from the opened device.
    uint32_t resolvedSize = 0;

#if defined(THL_PLATFORM_ANDROID)
    {
        auto* pStream = device->aaudio.pStreamPlayback
                            ? device->aaudio.pStreamPlayback
                            : device->aaudio.pStreamCapture;

        int32_t actualCapacity = 0;
        int32_t actualBurst = 0;
        int32_t actualFPDC = 0;
        if (pStream) {
            if (m_impl->context.aaudio.AAudioStream_getBufferCapacityInFrames) {
                actualCapacity = reinterpret_cast<int32_t (*)(void*)>(
                    m_impl->context.aaudio.AAudioStream_getBufferCapacityInFrames)(pStream);
            }
            if (m_impl->context.aaudio.AAudioStream_getFramesPerBurst) {
                actualBurst = reinterpret_cast<int32_t (*)(void*)>(
                    m_impl->context.aaudio.AAudioStream_getFramesPerBurst)(pStream);
            }
            if (m_impl->context.aaudio.AAudioStream_getFramesPerDataCallback) {
                actualFPDC = reinterpret_cast<int32_t (*)(void*)>(
                    m_impl->context.aaudio.AAudioStream_getFramesPerDataCallback)(pStream);
            }
        }

        uint32_t maPeriod = device->playback.internalPeriodSizeInFrames;
        if (maPeriod == 0) maPeriod = device->capture.internalPeriodSizeInFrames;

        // Use the configured periodSize (which may be a multiple of burst).
        // Fall back to AAudio burst, then miniaudio's internalPeriodSizeInFrames.
        resolvedSize = devConfig.periodSizeInFrames;
        if (resolvedSize == 0) resolvedSize = (actualBurst > 0) ? static_cast<uint32_t>(actualBurst) : maPeriod;
        if (resolvedSize == 0) resolvedSize = burstSize;

        thl::Logger::logf(thl::Logger::LogLevel::Debug,
                           "thl.audio_io.audio_device_manager",
                           "AAudio %s: burst=%d, capacity=%d, framesPerDataCallback=%d, "
                           "ma internalPeriodSize=%u, configuredPeriodSize=%u → periodSize=%u",
                           label, actualBurst, actualCapacity, actualFPDC,
                           maPeriod, devConfig.periodSizeInFrames, resolvedSize);
    }
#else
    {
        // On Apple / other platforms, use internalPeriodSizeInFrames directly.
        resolvedSize = device->playback.internalPeriodSizeInFrames;
        if (resolvedSize == 0) resolvedSize = device->capture.internalPeriodSizeInFrames;
        if (resolvedSize == 0) resolvedSize = bufferSizeInFrames;
    }
#endif

    thl::Logger::logf(thl::Logger::LogLevel::Debug,
                       "thl.audio_io.audio_device_manager",
                       "Initialised %s device '%s': sampleRate=%u, periodSize=%u, periods=%u, "
                       "channels(out=%u, in=%u)",
                       label,
                       (role == DeviceRole::Playback || role == DeviceRole::Duplex)
                           ? outputDevice->name.c_str() : inputDevice->name.c_str(),
                       sampleRate, resolvedSize, configuredPeriods,
                       numOutputChannels, numInputChannels);

    if (burstSize > 0 && m_impl->burstSize == 0) {
        m_impl->burstSize = burstSize;
    }

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
                      "Initialising AudioDeviceManager with sampleRate=%u, bufferSizeInFrames=%u, numInputChannels=%u, numOutputChannels=%u",
                      sampleRate, bufferSizeInFrames, numInputChannels, numOutputChannels);

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
    m_impl->burstSize = 0;

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
        m_impl->playbackActualPeriodSize.store(0, std::memory_order_relaxed);
    }
    if (m_impl->captureDeviceInitialised) {
        ma_device_uninit(&m_impl->captureDevice);
        m_impl->captureDeviceInitialised = false;
        m_impl->captureActualPeriodSize.store(0, std::memory_order_relaxed);
    }
    if (m_impl->duplexDeviceInitialised) {
        ma_device_uninit(&m_impl->duplexDevice);
        m_impl->duplexDeviceInitialised = false;
        m_impl->duplexActualPeriodSize.store(0, std::memory_order_relaxed);
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
    // Prefer the actual callback frame count resolved from the first callback.
    if (m_impl->playbackDeviceInitialised) {
        uint32_t actual = m_impl->playbackActualPeriodSize.load(std::memory_order_relaxed);
        return actual > 0 ? actual : m_impl->playbackPeriodSize;
    }
    if (m_impl->duplexDeviceInitialised) {
        uint32_t actual = m_impl->duplexActualPeriodSize.load(std::memory_order_relaxed);
        return actual > 0 ? actual : m_impl->duplexPeriodSize;
    }
    if (m_impl->captureDeviceInitialised) {
        uint32_t actual = m_impl->captureActualPeriodSize.load(std::memory_order_relaxed);
        return actual > 0 ? actual : m_impl->capturePeriodSize;
    }
    return m_bufferSize;
}

uint32_t AudioDeviceManager::getPeriodCount() const {
    if (m_impl->playbackDeviceInitialised) { return m_impl->playbackPeriods; }
    if (m_impl->duplexDeviceInitialised) { return m_impl->duplexPeriods; }
    if (m_impl->captureDeviceInitialised) { return m_impl->capturePeriods; }
    return 1;
}

uint32_t AudioDeviceManager::getBurstSize() const {
    if (m_impl->burstSize > 0) { return m_impl->burstSize; }
    return getPeriodSize();
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
    std::atomic<uint32_t>* actualPeriodSize = nullptr;

    switch (role) {
        case DeviceRole::Playback:
            callbacks = &m_playbackCallbacks;
            audioThreadRegistered = &m_playbackAudioThreadRegistered;
            preparedBufferSize = m_impl->playbackPeriodSize;
            actualPeriodSize = &m_impl->playbackActualPeriodSize;
            break;
        case DeviceRole::Capture:
            callbacks = &m_captureCallbacks;
            audioThreadRegistered = &m_captureAudioThreadRegistered;
            preparedBufferSize = m_impl->capturePeriodSize;
            actualPeriodSize = &m_impl->captureActualPeriodSize;
            break;
        case DeviceRole::Duplex:
            callbacks = &m_duplexCallbacks;
            audioThreadRegistered = &m_duplexAudioThreadRegistered;
            preparedBufferSize = m_impl->duplexPeriodSize;
            actualPeriodSize = &m_impl->duplexActualPeriodSize;
            break;
    }

    if (!device || !callbacks || !audioThreadRegistered) { return; }

    // Record the true period size from the first callback for reporting via
    // getPeriodSize(). On iOS, CoreAudio's Audio Unit render callback may
    // deliver fewer frames than what internalPeriodSizeInFrames / AVAudioSession
    // report.
    if (actualPeriodSize && actualPeriodSize->load(std::memory_order_relaxed) == 0) {
        actualPeriodSize->store(frameCount, std::memory_order_relaxed);
        if (frameCount != preparedBufferSize) {
            thl::Logger::logf(thl::Logger::LogLevel::Warning,
                               "thl.audio_io.audio_device_manager",
                               "Actual callback frameCount %u differs from configured periodSize %u "
                               "(role %d)",
                               frameCount, preparedBufferSize,
                               static_cast<int>(role));
        }
    }

    if (!audioThreadRegistered->load(std::memory_order_relaxed)) [[unlikely]] {
        callbacks->register_reader_thread();
        audioThreadRegistered->store(true, std::memory_order_relaxed);
    }

    uint32_t outputChannels = device->playback.channels;
    uint32_t inputChannels = device->capture.channels;

    if (outputChannels == 0 && inputChannels == 0) { return; }

#if defined(THL_DEBUG) && !defined(THL_PLATFORM_IOS_SIMULATOR)
    {
        static uint32_t callbackCounter = 0;
        bool mismatch = (frameCount != actualPeriodSize->load(std::memory_order_relaxed));
        if (mismatch) {
            thl::Logger::logf(thl::Logger::LogLevel::Warning,
                               "thl.audio_io.audio_device_manager",
                               "Callback #%u: frameCount %u, prepared %u, at last iteration %u (role %d)",
                               callbackCounter, frameCount, preparedBufferSize,
                               actualPeriodSize->load(std::memory_order_relaxed),
                               static_cast<int>(role));
            actualPeriodSize->store(frameCount, std::memory_order_relaxed);
        }
        ++callbackCounter;
    }
#endif

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
