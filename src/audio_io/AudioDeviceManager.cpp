#include <tanh/audio_io/AudioDeviceManager.h>
#include "miniaudio.h"
#include "tanh/core/Logger.h"
#include <algorithm>
#include <cstring>
#include <vector>

#if defined(THL_PLATFORM_MACOS) || defined(THL_PLATFORM_IOS)
#include <TargetConditionals.h>
#endif

#if defined(THL_PLATFORM_IOS)
#import <AVFoundation/AVAudioSession.h>
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

    // Maximum per-callback period size that callbacks were prepared with.
    uint32_t playbackPreparedPeriodSize = 512;
    uint32_t capturePreparedPeriodSize = 512;
    uint32_t duplexPreparedPeriodSize = 512;

    // Number of periods (Android: bufferSize / burst, iOS: 1).
    uint32_t playbackPeriods = 1;
    uint32_t capturePeriods = 1;
    uint32_t duplexPeriods = 1;

    // Hardware burst size (Android: AAudio framesPerBurst, Apple: same as periodSize).
    uint32_t playbackBurstSize = 0;
    uint32_t captureBurstSize = 0;
    uint32_t duplexBurstSize = 0;

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

#if defined(THL_PLATFORM_IOS)
namespace {
bool isBluetoothRouteActive() {
    AVAudioSession* session = [AVAudioSession sharedInstance];
    for (AVAudioSessionPortDescription* port in session.currentRoute.outputs) {
        if ([port.portType isEqualToString:AVAudioSessionPortBluetoothA2DP] ||
            [port.portType isEqualToString:AVAudioSessionPortBluetoothHFP] ||
            [port.portType isEqualToString:AVAudioSessionPortBluetoothLE]) {
            return true;
        }
    }
    for (AVAudioSessionPortDescription* port in session.currentRoute.inputs) {
        if ([port.portType isEqualToString:AVAudioSessionPortBluetoothHFP]) {
            return true;
        }
    }
    return false;
}
}  // namespace
#endif

uint32_t AudioDeviceManager::clampBufferSizeForBluetoothRoute(uint32_t bufferSizeInFrames,
                                                              uint32_t sampleRate) {
    if (sampleRate == 0) return bufferSizeInFrames;
    uint32_t maxFrames = static_cast<uint32_t>(
        static_cast<float>(sampleRate) * kMaxBluetoothIOBufferDurationSeconds);
    if (maxFrames == 0) maxFrames = 1;
    return (bufferSizeInFrames > maxFrames) ? maxFrames : bufferSizeInFrames;
}

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
    ctxConfig.coreaudio.sessionCategoryOptions =
        ma_ios_session_category_option_default_to_speaker |
        ma_ios_session_category_option_allow_bluetooth_a2dp;
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
                                     const ma_device_config& devConfig) {
    ma_device_config probeConfig = devConfig;
    probeConfig.periodSizeInFrames = 0;
    probeConfig.noFixedSizedCallback = MA_TRUE;
    probeConfig.aaudio.allowSetBufferCapacity = MA_FALSE;
    probeConfig.dataCallback = nullptr;
    probeConfig.notificationCallback = nullptr;
    probeConfig.pUserData = nullptr;

    uint32_t burstSize = 0;
    ma_device probeDevice;
    if (ma_device_init(&context, &probeConfig, &probeDevice) == MA_SUCCESS) {
        auto* pStream = (devConfig.deviceType == ma_device_type_capture)
                            ? probeDevice.aaudio.pStreamCapture
                            : probeDevice.aaudio.pStreamPlayback;

        if (pStream && context.aaudio.AAudioStream_getFramesPerBurst) {
            int32_t burst = reinterpret_cast<int32_t (*)(void*)>(
                context.aaudio.AAudioStream_getFramesPerBurst)(pStream);
            if (burst > 0) burstSize = static_cast<uint32_t>(burst);
        }

        ma_device_uninit(&probeDevice);
    }

    return burstSize;
}

struct AndroidBufferConfig {
    uint32_t periodSize = 0;
    uint32_t periods = 1;
    uint32_t burstSize = 0;
};

static AndroidBufferConfig configureAndroidBuffering(ma_context& context,
                                                     ma_device_config& devConfig,
                                                     uint32_t bufferSizeInFrames) {
    AndroidBufferConfig result;
    result.burstSize = probeAAudioBurstSize(context, devConfig);

    if (result.burstSize > 0) {
        if (bufferSizeInFrames == 0) bufferSizeInFrames = result.burstSize * 4;

        // Round up to nearest power-of-two multiple of burst.
        uint32_t m = bufferSizeInFrames / result.burstSize;
        if (m == 0) m = 1;
        uint32_t pot = 1;
        while (pot < m) pot <<= 1;
        m = pot;

        if (m <= 2) {
            result.periodSize = result.burstSize;
            result.periods = m;
        } else {
            result.periodSize = result.burstSize * (m / 2);
            result.periods = 2;
        }
    } else {
        result.periodSize = bufferSizeInFrames > 0 ? bufferSizeInFrames : 256;
    }

    return result;
}

static uint32_t resolveAAudioPeriodSize(ma_context& context,
                                        ma_device& device,
                                        ma_device_type deviceType) {
    auto* pStream = (deviceType == ma_device_type_capture)
                        ? device.aaudio.pStreamCapture
                        : device.aaudio.pStreamPlayback;

    if (pStream && context.aaudio.AAudioStream_getFramesPerDataCallback) {
        int32_t fpdc = reinterpret_cast<int32_t (*)(void*)>(
            context.aaudio.AAudioStream_getFramesPerDataCallback)(pStream);
        if (fpdc > 0) return static_cast<uint32_t>(fpdc);
    }

    return 0;
}

#elif defined(THL_PLATFORM_IOS) || defined(THL_PLATFORM_MACOS)

struct CoreAudioProbeResult {
    uint32_t periodSize = 0;
    uint32_t periods = 0;
};

static CoreAudioProbeResult probeCoreAudioPeriod(ma_context& context,
                                                 const ma_device_config& devConfig,
                                                 uint32_t requestedSize) {
    ma_device_config probeConfig = devConfig;
    probeConfig.periodSizeInFrames = requestedSize;
    probeConfig.periods = 1;
    probeConfig.noFixedSizedCallback = MA_TRUE;
    probeConfig.dataCallback = nullptr;
    probeConfig.notificationCallback = nullptr;
    probeConfig.pUserData = nullptr;

    CoreAudioProbeResult result;
    ma_device probeDevice;
    if (ma_device_init(&context, &probeConfig, &probeDevice) == MA_SUCCESS) {
        if (devConfig.deviceType == ma_device_type_capture) {
            result.periodSize = probeDevice.capture.internalPeriodSizeInFrames;
            result.periods = probeDevice.capture.internalPeriods;
        } else {
            result.periodSize = probeDevice.playback.internalPeriodSizeInFrames;
            result.periods = probeDevice.playback.internalPeriods;
        }
        ma_device_uninit(&probeDevice);
    }
    return result;
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

    // --- Build device config ---
    ma_device_config devConfig = ma_device_config_init(deviceType);
    devConfig.sampleRate = sampleRate;
    devConfig.noFixedSizedCallback = MA_TRUE;
    devConfig.dataCallback =
        reinterpret_cast<ma_device_data_proc>(&AudioDeviceManager::dataCallback);
    devConfig.notificationCallback =
        reinterpret_cast<ma_device_notification_proc>(&AudioDeviceManager::notificationCallback);
    devConfig.pUserData = userData;
#if defined(THL_PLATFORM_ANDROID)
    devConfig.aaudio.allowSetBufferCapacity = MA_TRUE;
    devConfig.aaudio.usage = (m_bluetoothProfile == BluetoothProfile::A2DP)
                                 ? ma_aaudio_usage_media
                                 : ma_aaudio_usage_voice_communication;
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

    // --- Platform-specific buffer configuration ---
    uint32_t configuredPeriods = 1;
    uint32_t burstSize = 0;

#if defined(THL_PLATFORM_IOS)
    // Bluetooth HFP/SCO delivers silent capture buffers when the
    // AVAudioSession IO buffer duration exceeds ~64 ms.  Clamp the
    // requested size so miniaudio never sets a duration beyond this
    // limit when a Bluetooth route is active.
    if (isBluetoothRouteActive()) {
        uint32_t clamped = clampBufferSizeForBluetoothRoute(bufferSizeInFrames, sampleRate);
        if (clamped != bufferSizeInFrames) {
            thl::Logger::logf(thl::Logger::LogLevel::Warning,
                               "thl.audio_io.audio_device_manager",
                               "%s: Bluetooth route active — clamping buffer from %u to %u frames "
                               "(max %.0f ms)",
                               label, bufferSizeInFrames, clamped,
                               kMaxBluetoothIOBufferDurationSeconds * 1000.0f);
            bufferSizeInFrames = clamped;
        }
    }
#endif

#if defined(THL_PLATFORM_ANDROID)
    {
        auto probe = configureAndroidBuffering(
            m_impl->context, devConfig, bufferSizeInFrames);

        burstSize = probe.burstSize;
        devConfig.periodSizeInFrames = probe.periodSize;
        configuredPeriods = probe.periods;
        devConfig.periods = probe.periods;

        thl::Logger::logf(thl::Logger::LogLevel::Debug,
                           "thl.audio_io.audio_device_manager",
                           "%s: probed burstSize=%u, configured periodSize=%u, periods=%u",
                           label, burstSize, probe.periodSize, probe.periods);
    }
#elif defined(THL_PLATFORM_IOS) || defined(THL_PLATFORM_MACOS)
    {
        auto probe = probeCoreAudioPeriod(
            m_impl->context, devConfig, bufferSizeInFrames);

        if (probe.periodSize > 0 && probe.periodSize != bufferSizeInFrames) {
            thl::Logger::logf(thl::Logger::LogLevel::Warning,
                               "thl.audio_io.audio_device_manager",
                               "%s: probed periodSize=%u differs from requested %u, adjusting",
                               label, probe.periodSize, bufferSizeInFrames);
        }

        burstSize = probe.periodSize;
        devConfig.periodSizeInFrames = probe.periodSize;
        configuredPeriods = (probe.periods > 0) ? probe.periods : 1;
        devConfig.periods = configuredPeriods;

        thl::Logger::logf(thl::Logger::LogLevel::Debug,
                           "thl.audio_io.audio_device_manager",
                           "%s: probed periodSize=%u, periods=%u",
                           label, probe.periodSize, configuredPeriods);
    }
#else
    devConfig.periodSizeInFrames = bufferSizeInFrames;
#endif

    // --- Open device ---
    if (ma_device_init(&m_impl->context, &devConfig, device) != MA_SUCCESS) { return false; }

    // --- Resolve actual period size ---
    uint32_t resolvedPeriodSize = 0;
#if defined(THL_PLATFORM_ANDROID)
    resolvedPeriodSize = resolveAAudioPeriodSize(m_impl->context, *device, deviceType);
    if (resolvedPeriodSize == 0) resolvedPeriodSize = devConfig.periodSizeInFrames;
#else
    if (role == DeviceRole::Capture) {
        resolvedPeriodSize = device->capture.internalPeriodSizeInFrames;
    } else {
        resolvedPeriodSize = device->playback.internalPeriodSizeInFrames;
    }
    if (resolvedPeriodSize == 0) resolvedPeriodSize = bufferSizeInFrames;
#endif

    thl::Logger::logf(thl::Logger::LogLevel::Debug,
                       "thl.audio_io.audio_device_manager",
                       "%s: initialised device '%s', sampleRate=%u, periodSize=%u, periods=%u, "
                       "channels(out=%u, in=%u)",
                       label,
                       (role == DeviceRole::Playback || role == DeviceRole::Duplex)
                           ? outputDevice->name.c_str() : inputDevice->name.c_str(),
                       sampleRate, resolvedPeriodSize, configuredPeriods,
                       numOutputChannels, numInputChannels);

    // --- Store results ---
    switch (role) {
        case DeviceRole::Playback:
            m_impl->playbackDeviceInitialised = true;
            m_numOutputChannels = device->playback.channels;
            m_sampleRate = device->sampleRate;
            m_impl->playbackPreparedPeriodSize = resolvedPeriodSize;
            m_impl->playbackPeriods = configuredPeriods;
            m_impl->playbackBurstSize = burstSize;
            break;
        case DeviceRole::Capture:
            m_impl->captureDeviceInitialised = true;
            m_numInputChannels = device->capture.channels;
            if (!m_impl->playbackDeviceInitialised) { m_sampleRate = device->sampleRate; }
            m_impl->capturePreparedPeriodSize = resolvedPeriodSize;
            m_impl->capturePeriods = configuredPeriods;
            m_impl->captureBurstSize = burstSize;
            break;
        case DeviceRole::Duplex:
            m_impl->duplexDeviceInitialised = true;
            if (!m_impl->playbackDeviceInitialised && !m_impl->captureDeviceInitialised) {
                m_sampleRate = device->sampleRate;
            }
            m_impl->duplexPreparedPeriodSize = resolvedPeriodSize;
            m_impl->duplexPeriods = configuredPeriods;
            m_impl->duplexBurstSize = burstSize;
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
    m_numInputChannels = inputDevice ? numInputChannels : 0;
    m_numOutputChannels = outputDevice ? numOutputChannels : 0;
    m_impl->playbackPreparedPeriodSize = bufferSizeInFrames;
    m_impl->capturePreparedPeriodSize = bufferSizeInFrames;
    m_impl->duplexPreparedPeriodSize = bufferSizeInFrames;
    m_impl->playbackPeriods = 1;
    m_impl->capturePeriods = 1;
    m_impl->duplexPeriods = 1;
    m_impl->playbackBurstSize = 0;
    m_impl->captureBurstSize = 0;
    m_impl->duplexBurstSize = 0;

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

    auto prepareCallbacks =
        [](RCU<std::vector<AudioIODeviceCallback*>>& list, ma_device& device, uint32_t bufferSize) {
            list.read([&](const auto& callbacks) {
                for (auto* callback : callbacks) {
                    callback->prepareToPlay(device.sampleRate, bufferSize);
                }
            });
        };

    if (m_impl->playbackDeviceInitialised) {
        prepareCallbacks(m_playbackCallbacks, m_impl->playbackDevice, m_impl->playbackPreparedPeriodSize);
    }
    if (m_impl->captureDeviceInitialised) {
        prepareCallbacks(m_captureCallbacks, m_impl->captureDevice, m_impl->capturePreparedPeriodSize);
    }
    if (m_impl->duplexDeviceInitialised) {
        prepareCallbacks(m_duplexCallbacks, m_impl->duplexDevice, m_impl->duplexPreparedPeriodSize);
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
        callback->prepareToPlay(m_impl->playbackDevice.sampleRate, m_impl->playbackPreparedPeriodSize);
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
        callback->prepareToPlay(m_impl->captureDevice.sampleRate, m_impl->capturePreparedPeriodSize);
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
        callback->prepareToPlay(m_impl->duplexDevice.sampleRate, m_impl->duplexPreparedPeriodSize);
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

bool AudioDeviceManager::setBluetoothProfile(BluetoothProfile profile) {
    if (m_playbackRunning || m_captureRunning || m_duplexRunning) {
        thl::Logger::warning("thl.audio_io.audio_device_manager",
                             "setBluetoothProfile: cannot change while devices are running");
        return false;
    }

    m_bluetoothProfile = profile;

#if defined(THL_PLATFORM_IOS)
    AVAudioSession* session = [AVAudioSession sharedInstance];

    AVAudioSessionCategoryOptions options =
        AVAudioSessionCategoryOptionDefaultToSpeaker;

    if (profile == BluetoothProfile::A2DP) {
        options |= AVAudioSessionCategoryOptionAllowBluetoothA2DP;
    } else {
        options |= AVAudioSessionCategoryOptionAllowBluetooth;
    }

    NSError* error = nil;
    BOOL ok = [session setCategory:AVAudioSessionCategoryPlayAndRecord
                       withOptions:options
                             error:&error];

    if (!ok) {
        thl::Logger::logf(thl::Logger::LogLevel::Error,
                           "thl.audio_io.audio_device_manager",
                           "setBluetoothProfile: failed to set session category — %s",
                           error ? [[error localizedDescription] UTF8String] : "unknown error");
        return false;
    }

    ok = [session setActive:YES error:&error];
    if (!ok) {
        thl::Logger::logf(thl::Logger::LogLevel::Error,
                           "thl.audio_io.audio_device_manager",
                           "setBluetoothProfile: failed to activate session — %s",
                           error ? [[error localizedDescription] UTF8String] : "unknown error");
        return false;
    }

    thl::Logger::logf(thl::Logger::LogLevel::Info,
                       "thl.audio_io.audio_device_manager",
                       "Bluetooth profile set to %s (session sampleRate=%.0f, IOBufferDuration=%.4f)",
                       profile == BluetoothProfile::A2DP ? "A2DP" : "HFP",
                       session.sampleRate,
                       session.IOBufferDuration);
#elif defined(THL_PLATFORM_ANDROID)
    // On Android the AAudio usage type (media vs voice_communication) is
    // applied per-device at init time via tryInitialiseDevice().  Storing
    // the profile here is sufficient — the next initialise() call will
    // pick it up.
    thl::Logger::logf(thl::Logger::LogLevel::Info,
                       "thl.audio_io.audio_device_manager",
                       "Bluetooth profile set to %s (will take effect on next initialise)",
                       profile == BluetoothProfile::A2DP ? "A2DP" : "HFP");
#else
    (void)profile;
#endif

    return true;
}

BluetoothProfile AudioDeviceManager::getBluetoothProfile() const {
    return m_bluetoothProfile;
}

uint32_t AudioDeviceManager::getSampleRate() const {
    if (m_impl->playbackDeviceInitialised) { return m_impl->playbackDevice.sampleRate; }
    if (m_impl->duplexDeviceInitialised) { return m_impl->duplexDevice.sampleRate; }
    if (m_impl->captureDeviceInitialised) { return m_impl->captureDevice.sampleRate; }
    return m_sampleRate;
}

uint32_t AudioDeviceManager::getBufferSize() const {
#if defined(THL_PLATFORM_ANDROID)
    if (m_impl->playbackDeviceInitialised)
        return m_impl->playbackPreparedPeriodSize * m_impl->playbackPeriods;
    if (m_impl->duplexDeviceInitialised)
        return m_impl->duplexPreparedPeriodSize * m_impl->duplexPeriods;
    if (m_impl->captureDeviceInitialised)
        return m_impl->capturePreparedPeriodSize * m_impl->capturePeriods;
#else
    if (m_impl->playbackDeviceInitialised) return m_impl->playbackPreparedPeriodSize;
    if (m_impl->duplexDeviceInitialised) return m_impl->duplexPreparedPeriodSize;
    if (m_impl->captureDeviceInitialised) return m_impl->capturePreparedPeriodSize;
#endif
    return 0;
}

uint32_t AudioDeviceManager::getBufferSize(DeviceRole role) const {
    switch (role) {
        case DeviceRole::Playback:
            if (m_impl->playbackDeviceInitialised)
#if defined(THL_PLATFORM_ANDROID)
                return m_impl->playbackPreparedPeriodSize * m_impl->playbackPeriods;
#else
                return m_impl->playbackPreparedPeriodSize;
#endif
            break;
        case DeviceRole::Capture:
            if (m_impl->captureDeviceInitialised)
#if defined(THL_PLATFORM_ANDROID)
                return m_impl->capturePreparedPeriodSize * m_impl->capturePeriods;
#else
                return m_impl->capturePreparedPeriodSize;
#endif
            break;
        case DeviceRole::Duplex:
            if (m_impl->duplexDeviceInitialised)
#if defined(THL_PLATFORM_ANDROID)
                return m_impl->duplexPreparedPeriodSize * m_impl->duplexPeriods;
#else
                return m_impl->duplexPreparedPeriodSize;
#endif
            break;
    }
    return getBufferSize();
}

uint32_t AudioDeviceManager::getPeriodSize() const {
    // Prefer the actual callback frame count resolved from the first callback.
    if (m_impl->playbackDeviceInitialised) {
        uint32_t actual = m_impl->playbackActualPeriodSize.load(std::memory_order_relaxed);
        return actual > 0 ? actual : m_impl->playbackPreparedPeriodSize;
    }
    if (m_impl->duplexDeviceInitialised) {
        uint32_t actual = m_impl->duplexActualPeriodSize.load(std::memory_order_relaxed);
        return actual > 0 ? actual : m_impl->duplexPreparedPeriodSize;
    }
    if (m_impl->captureDeviceInitialised) {
        uint32_t actual = m_impl->captureActualPeriodSize.load(std::memory_order_relaxed);
        return actual > 0 ? actual : m_impl->capturePreparedPeriodSize;
    }
    return getBufferSize();
}

uint32_t AudioDeviceManager::getPeriodSize(DeviceRole role) const {
    switch (role) {
        case DeviceRole::Playback:
            if (m_impl->playbackDeviceInitialised) {
                uint32_t actual = m_impl->playbackActualPeriodSize.load(std::memory_order_relaxed);
                return actual > 0 ? actual : m_impl->playbackPreparedPeriodSize;
            }
            break;
        case DeviceRole::Capture:
            if (m_impl->captureDeviceInitialised) {
                uint32_t actual = m_impl->captureActualPeriodSize.load(std::memory_order_relaxed);
                return actual > 0 ? actual : m_impl->capturePreparedPeriodSize;
            }
            break;
        case DeviceRole::Duplex:
            if (m_impl->duplexDeviceInitialised) {
                uint32_t actual = m_impl->duplexActualPeriodSize.load(std::memory_order_relaxed);
                return actual > 0 ? actual : m_impl->duplexPreparedPeriodSize;
            }
            break;
    }
    return getPeriodSize();
}

uint32_t AudioDeviceManager::getPeriodCount() const {
    if (m_impl->playbackDeviceInitialised) { return m_impl->playbackPeriods; }
    if (m_impl->duplexDeviceInitialised) { return m_impl->duplexPeriods; }
    if (m_impl->captureDeviceInitialised) { return m_impl->capturePeriods; }
    return 1;
}

uint32_t AudioDeviceManager::getPeriodCount(DeviceRole role) const {
    switch (role) {
        case DeviceRole::Playback:
            if (m_impl->playbackDeviceInitialised) return m_impl->playbackPeriods;
            break;
        case DeviceRole::Capture:
            if (m_impl->captureDeviceInitialised) return m_impl->capturePeriods;
            break;
        case DeviceRole::Duplex:
            if (m_impl->duplexDeviceInitialised) return m_impl->duplexPeriods;
            break;
    }
    return 1;
}

uint32_t AudioDeviceManager::getBurstSize() const {
    if (m_impl->playbackDeviceInitialised && m_impl->playbackBurstSize > 0) return m_impl->playbackBurstSize;
    if (m_impl->duplexDeviceInitialised && m_impl->duplexBurstSize > 0) return m_impl->duplexBurstSize;
    if (m_impl->captureDeviceInitialised && m_impl->captureBurstSize > 0) return m_impl->captureBurstSize;
    return getPeriodSize();
}

uint32_t AudioDeviceManager::getBurstSize(DeviceRole role) const {
    switch (role) {
        case DeviceRole::Playback:
            if (m_impl->playbackDeviceInitialised && m_impl->playbackBurstSize > 0)
                return m_impl->playbackBurstSize;
            break;
        case DeviceRole::Capture:
            if (m_impl->captureDeviceInitialised && m_impl->captureBurstSize > 0)
                return m_impl->captureBurstSize;
            break;
        case DeviceRole::Duplex:
            if (m_impl->duplexDeviceInitialised && m_impl->duplexBurstSize > 0)
                return m_impl->duplexBurstSize;
            break;
    }
    return getPeriodSize(role);
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
    if (!device) { return; }

    // Select per-role state.
    RCU<std::vector<AudioIODeviceCallback*>>* callbacks = nullptr;
    std::atomic<bool>* audioThreadRegistered = nullptr;
    uint32_t maxChunkSize = 0;
    std::atomic<uint32_t>* actualPeriodSize = nullptr;

    switch (role) {
        case DeviceRole::Playback:
            callbacks = &m_playbackCallbacks;
            audioThreadRegistered = &m_playbackAudioThreadRegistered;
            maxChunkSize = m_impl->playbackPreparedPeriodSize;
            actualPeriodSize = &m_impl->playbackActualPeriodSize;
            break;
        case DeviceRole::Capture:
            callbacks = &m_captureCallbacks;
            audioThreadRegistered = &m_captureAudioThreadRegistered;
            maxChunkSize = m_impl->capturePreparedPeriodSize;
            actualPeriodSize = &m_impl->captureActualPeriodSize;
            break;
        case DeviceRole::Duplex:
            callbacks = &m_duplexCallbacks;
            audioThreadRegistered = &m_duplexAudioThreadRegistered;
            maxChunkSize = m_impl->duplexPreparedPeriodSize;
            actualPeriodSize = &m_impl->duplexActualPeriodSize;
            break;
    }

    if (!callbacks || !audioThreadRegistered) { return; }

    // Record the true period size from the first callback for reporting via
    // getPeriodSize(). On iOS the actual AU render callback may deliver fewer
    // frames than internalPeriodSizeInFrames / AVAudioSession report.
    if (actualPeriodSize->load(std::memory_order_relaxed) == 0) {
        actualPeriodSize->store(frameCount, std::memory_order_relaxed);
        if (frameCount != maxChunkSize) {
            thl::Logger::logf(thl::Logger::LogLevel::Warning,
                               "thl.audio_io.audio_device_manager",
                               "Actual first callback frameCount %u differs from prepared periodSize %u "
                               "(role %d)",
                               frameCount, maxChunkSize,
                               static_cast<int>(role));
        }
    }

    // One-time RCU reader registration for this audio thread.
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
        if (frameCount != actualPeriodSize->load(std::memory_order_relaxed)) {
            thl::Logger::logf(thl::Logger::LogLevel::Warning,
                               "thl.audio_io.audio_device_manager",
                               "Callback #%u: frameCount %u, prepared %u, previous %u (role %d)",
                               callbackCounter, frameCount, maxChunkSize,
                               actualPeriodSize->load(std::memory_order_relaxed),
                               static_cast<int>(role));
            actualPeriodSize->store(frameCount, std::memory_order_relaxed);
        }
        // if (callbackCounter % 100 == 0) {
        //     thl::Logger::logf(thl::Logger::LogLevel::Debug,
        //                        "thl.audio_io.audio_device_manager",
        //                        "Callback #%u: frameCount %u, prepared %u, previous %u (role %d)",
        //                        callbackCounter, frameCount, maxChunkSize, actualPeriodSize->load(std::memory_order_relaxed),
        //                        static_cast<int>(role));
        // }
        ++callbackCounter;
    }
#endif

    // Zero output so callbacks can additively mix.
    if (output && outputChannels > 0) {
        std::memset(output, 0, frameCount * outputChannels * sizeof(float));
    }

    float* safeOutput = outputChannels > 0 ? output : nullptr;
    const float* safeInput = inputChannels > 0 ? input : nullptr;

    // Process in chunks no larger than preparedPeriodSize to avoid overflowing
    // internal DSP buffers allocated by prepareToPlay().
    uint32_t framesRemaining = frameCount;
    uint32_t offset = 0;

    while (framesRemaining > 0) {
        uint32_t chunk = (maxChunkSize > 0)
                             ? std::min(framesRemaining, maxChunkSize)
                             : framesRemaining;

        float* chunkOut = safeOutput ? safeOutput + offset * outputChannels : nullptr;
        const float* chunkIn = safeInput ? safeInput + offset * inputChannels : nullptr;

        callbacks->read([&](const auto& list) {
            for (auto* cb : list) {
                cb->process(chunkOut, chunkIn, chunk, inputChannels, outputChannels);
            }
        });

        offset += chunk;
        framesRemaining -= chunk;
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
