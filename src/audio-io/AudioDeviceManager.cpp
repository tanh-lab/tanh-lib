#include <tanh/audio-io/AudioDeviceManager.h>
#include "miniaudio.h"
#include "tanh/core/Logger.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#if defined(THL_PLATFORM_ANDROID)
#include <tanh/audio-io/AndroidAudioDevices.h>
#endif

#if defined(THL_PLATFORM_MACOS) || defined(THL_PLATFORM_IOS)
#include <TargetConditionals.h>
#endif

#if defined(THL_PLATFORM_IOS)
#include <tanh/audio-io/iOSAudioDevices.h>
#endif

namespace thl {

struct AudioDeviceManager::DeviceUserData {
    AudioDeviceManager* m_manager = nullptr;
    AudioDeviceManager::DeviceRole m_role = AudioDeviceManager::DeviceRole::Playback;
};

struct AudioDeviceManager::Impl {
    ma_context m_context;
    ma_device m_playback_device;
    ma_device m_capture_device;
    ma_device m_duplex_device;

    bool m_context_initialised = false;
    bool m_playback_device_initialised = false;
    bool m_capture_device_initialised = false;
    bool m_duplex_device_initialised = false;

    // Maximum per-callback period size that callbacks were prepared with.
    uint32_t m_playback_prepared_period_size = 512;
    uint32_t m_capture_prepared_period_size = 512;
    uint32_t m_duplex_prepared_period_size = 512;

    // Number of periods (Android: bufferSize / burst, iOS: 1).
    uint32_t m_playback_periods = 1;
    uint32_t m_capture_periods = 1;
    uint32_t m_duplex_periods = 1;

    // Hardware burst size (Android: AAudio framesPerBurst, Apple: same as periodSize).
    uint32_t m_playback_burst_size = 0;
    uint32_t m_capture_burst_size = 0;
    uint32_t m_duplex_burst_size = 0;

    // On iOS, the actual Audio Unit render callback frame count can differ
    // from what AVAudioSession / MaximumFramesPerSlice report.
    // These atomics are written once from the first callback and read by
    // getPeriodSize() / getBurstSize().
    std::atomic<uint32_t> m_playback_actual_period_size{0};
    std::atomic<uint32_t> m_capture_actual_period_size{0};
    std::atomic<uint32_t> m_duplex_actual_period_size{0};

    // Capture rate measurement — used on Android to detect the actual SCO
    // sample rate by timing callbacks.  Written from the audio thread,
    // read from the main thread via getCaptureSampleRate().
    std::atomic<uint64_t> m_capture_first_callback_us{0};  // microseconds since epoch
    std::atomic<uint64_t> m_capture_total_frames{0};
    std::atomic<uint32_t> m_capture_measured_rate{0};              // 0 = not yet measured
    static constexpr uint64_t k_rate_calibration_frames = 16000;  // ~1 s at 16 kHz

    DeviceUserData m_playback_user_data;
    DeviceUserData m_capture_user_data;
    DeviceUserData m_duplex_user_data;

    LogCallback m_log_callback;

    // Device names passed to initialise()
    std::string m_output_device_name;
    std::string m_input_device_name;
};

namespace {

DeviceNotificationType to_notification_type(ma_device_notification_type type) {
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

DeviceType to_device_type(ma_device_type type) {
    switch (type) {
        case ma_device_type_playback: return DeviceType::Playback;
        case ma_device_type_capture: return DeviceType::Capture;
        case ma_device_type_duplex: return DeviceType::Duplex;
        default: return DeviceType::Playback;
    }
}

ma_device_type to_ma_device_type(DeviceType type) {
    switch (type) {
        case DeviceType::Playback: return ma_device_type_playback;
        case DeviceType::Capture: return ma_device_type_capture;
        case DeviceType::Duplex: return ma_device_type_duplex;
    }
    return ma_device_type_playback;
}

struct EnumUserData {
    std::vector<AudioDeviceInfo>* m_devices;
    ma_device_type m_target_type;
};

ma_bool32 enum_callback(ma_context* /*pContext*/,
                       ma_device_type device_type,
                       const ma_device_info* p_info,
                       void* p_user_data) {
    auto* user_data = static_cast<EnumUserData*>(p_user_data);
    if (device_type != user_data->m_target_type) { return MA_TRUE; }

    static_assert(sizeof(ma_device_id) <= AudioDeviceInfo::k_device_id_storage_size,
                  "ma_device_id too large for storage");

    AudioDeviceInfo info;
    info.m_name = p_info->name;
    info.m_device_type = to_device_type(device_type);
    std::memcpy(info.device_id_storage_ptr(), &p_info->id, sizeof(ma_device_id));
    user_data->m_devices->push_back(std::move(info));
    return MA_TRUE;
}

}  // namespace

uint32_t AudioDeviceManager::clamp_buffer_size_for_bluetooth_route(uint32_t buffer_size_in_frames,
                                                              uint32_t sample_rate) {
    if (sample_rate == 0) { return buffer_size_in_frames; }
    uint32_t max_frames = static_cast<uint32_t>(static_cast<float>(sample_rate) *
                                               k_max_bluetooth_io_buffer_duration_seconds);
    if (max_frames == 0) { max_frames = 1; }
    return (buffer_size_in_frames > max_frames) ? max_frames : buffer_size_in_frames;
}

AudioDeviceManager::AudioDeviceManager() : m_impl(std::make_unique<Impl>()) {
    m_impl->m_playback_user_data.m_manager = this;
    m_impl->m_playback_user_data.m_role = DeviceRole::Playback;
    m_impl->m_capture_user_data.m_manager = this;
    m_impl->m_capture_user_data.m_role = DeviceRole::Capture;
    m_impl->m_duplex_user_data.m_manager = this;
    m_impl->m_duplex_user_data.m_role = DeviceRole::Duplex;

    ma_context_config ctx_config = ma_context_config_init();

#if defined(THL_PLATFORM_IOS)
    configureIOSAudioSession();
    ctxConfig.coreaudio.sessionCategory = ma_ios_session_category_none;
#endif

#if defined(THL_PLATFORM_ANDROID)
    configureAndroidBluetoothSession();
#endif

    ma_result result = ma_context_init(nullptr, 0, &ctx_config, &m_impl->m_context);
    m_impl->m_context_initialised = (result == MA_SUCCESS);
}

AudioDeviceManager::~AudioDeviceManager() {
    shutdown();
    if (m_impl->m_context_initialised) {
        ma_context_uninit(&m_impl->m_context);
        m_impl->m_context_initialised = false;
    }
}

bool AudioDeviceManager::is_context_initialised() const {
    return m_impl->m_context_initialised;
}

void AudioDeviceManager::populate_sample_rates(std::vector<AudioDeviceInfo>& devices) const {
    for (auto& info : devices) {
        ma_device_info device_info;
        ma_device_id device_id{};
        std::memcpy(&device_id, info.device_id_ptr(), sizeof(device_id));
        ma_result result = ma_context_get_device_info(const_cast<ma_context*>(&m_impl->m_context),
                                                      to_ma_device_type(info.m_device_type),
                                                      &device_id,
                                                      &device_info);

        std::vector<uint32_t> rates;
        if (result == MA_SUCCESS) {
            for (ma_uint32 i = 0; i < device_info.nativeDataFormatCount; i++) {
                uint32_t rate = device_info.nativeDataFormats[i].sampleRate;
                if (rate > 0) {
                    if (std::find(rates.begin(), rates.end(), rate) == rates.end()) {
                        rates.push_back(rate);
                    }
                }
            }
        }

        if (rates.empty()) {
            ma_log* log = ma_context_get_log(const_cast<ma_context*>(&m_impl->m_context));
            if (log) {
                ma_log_postf(log,
                             MA_LOG_LEVEL_WARNING,
                             "AudioDeviceManager: No sample rates reported for "
                             "device '%s', using defaults",
                             info.m_name.c_str());
            }
            rates = k_default_sample_rates;
        }

        std::sort(rates.begin(), rates.end());
        info.m_sample_rates = std::move(rates);
    }
}

#if defined(THL_PLATFORM_ANDROID)
void AudioDeviceManager::setJavaVM(void* javaVM) {
    setAndroidJavaVM(javaVM);
}
#endif

std::vector<AudioDeviceInfo> AudioDeviceManager::enumerate_devices(DeviceType type) const {
    if (!m_impl->m_context_initialised) { return {}; }

#if defined(THL_PLATFORM_ANDROID)
    // Miniaudio's AAudio backend only reports default devices.
    // Use the Android AudioManager JNI bridge for real enumeration.
    {
        auto jniDevices = enumerateAndroidAudioDevices(type);
        if (!jniDevices.empty()) { return jniDevices; }
    }
    // Fall through to miniaudio as last-resort fallback.
#endif

    std::vector<AudioDeviceInfo> devices;
    EnumUserData user_data{&devices, to_ma_device_type(type)};
    ma_context_enumerate_devices(const_cast<ma_context*>(&m_impl->m_context),
                                 enum_callback,
                                 &user_data);
    populate_sample_rates(devices);
    return devices;
}

std::vector<AudioDeviceInfo> AudioDeviceManager::enumerate_input_devices() const {
    return enumerate_devices(DeviceType::Capture);
}

std::vector<AudioDeviceInfo> AudioDeviceManager::enumerate_output_devices() const {
    return enumerate_devices(DeviceType::Playback);
}

#if defined(THL_PLATFORM_ANDROID)
static uint32_t probeAAudioBurstSize(ma_context& context, const ma_device_config& devConfig) {
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
            if (burst > 0) { burstSize = static_cast<uint32_t>(burst); }
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
        if (bufferSizeInFrames == 0) { bufferSizeInFrames = result.burstSize * 4; }

        // Round up to nearest power-of-two multiple of burst.
        uint32_t m = bufferSizeInFrames / result.burstSize;
        if (m == 0) { m = 1; }
        uint32_t pot = 1;
        while (pot < m) { pot <<= 1; }
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
    auto* pStream = (deviceType == ma_device_type_capture) ? device.aaudio.pStreamCapture
                                                           : device.aaudio.pStreamPlayback;

    if (pStream && context.aaudio.AAudioStream_getFramesPerDataCallback) {
        int32_t fpdc = reinterpret_cast<int32_t (*)(void*)>(
            context.aaudio.AAudioStream_getFramesPerDataCallback)(pStream);
        if (fpdc > 0) { return static_cast<uint32_t>(fpdc); }
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

bool AudioDeviceManager::try_initialise_device(DeviceRole role,
                                             const AudioDeviceInfo* input_device,
                                             const AudioDeviceInfo* output_device,
                                             uint32_t sample_rate,
                                             uint32_t buffer_size_in_frames,
                                             uint32_t num_input_channels,
                                             uint32_t num_output_channels) {
    ma_device* device = nullptr;
    DeviceUserData* user_data = nullptr;
    ma_device_type device_type = ma_device_type_playback;
    const char* label = nullptr;

    switch (role) {
        case DeviceRole::Playback:
            if (!output_device) { return false; }
            device = &m_impl->m_playback_device;
            user_data = &m_impl->m_playback_user_data;
            device_type = ma_device_type_playback;
            label = "playback";
            break;
        case DeviceRole::Capture:
            if (!input_device) { return false; }
            device = &m_impl->m_capture_device;
            user_data = &m_impl->m_capture_user_data;
            device_type = ma_device_type_capture;
            label = "capture";
            break;
        case DeviceRole::Duplex:
            if (!input_device || !output_device) { return false; }
            device = &m_impl->m_duplex_device;
            user_data = &m_impl->m_duplex_user_data;
            device_type = ma_device_type_duplex;
            label = "duplex";
            break;
    }

    // --- Build device config ---
    ma_device_config dev_config = ma_device_config_init(device_type);
    dev_config.sampleRate = sample_rate;
    dev_config.noFixedSizedCallback = MA_TRUE;
    dev_config.dataCallback =
        reinterpret_cast<ma_device_data_proc>(&AudioDeviceManager::data_callback);
    dev_config.notificationCallback =
        reinterpret_cast<ma_device_notification_proc>(&AudioDeviceManager::notification_callback);
    dev_config.pUserData = user_data;
#if defined(THL_PLATFORM_ANDROID)
    dev_config.aaudio.allowSetBufferCapacity = MA_TRUE;
    dev_config.aaudio.usage = ma_aaudio_usage_media;
    dev_config.aaudio.contentType = ma_aaudio_content_type_music;
#endif

    ma_device_id output_device_id{};
    ma_device_id input_device_id{};
    if (role == DeviceRole::Playback || role == DeviceRole::Duplex) {
        std::memcpy(&output_device_id, output_device->device_id_ptr(), sizeof(output_device_id));
        dev_config.playback.pDeviceID = &output_device_id;
        dev_config.playback.format = ma_format_f32;
        dev_config.playback.channels = num_output_channels;
    }
    if (role == DeviceRole::Capture || role == DeviceRole::Duplex) {
        std::memcpy(&input_device_id, input_device->device_id_ptr(), sizeof(input_device_id));
        dev_config.capture.pDeviceID = &input_device_id;
        dev_config.capture.format = ma_format_f32;
        dev_config.capture.channels = num_input_channels;
    }

    // --- Platform-specific buffer configuration ---
    uint32_t configured_periods = 1;
    uint32_t burst_size = 0;

#if defined(THL_PLATFORM_IOS)
    // Bluetooth HFP/SCO delivers silent capture buffers when the
    // AVAudioSession IO buffer duration exceeds ~64 ms.  Clamp the
    // requested size so miniaudio never sets a duration beyond this
    // limit when a Bluetooth route is active.
    if (isIOSBluetoothRouteActive()) {
        uint32_t clamped = clampBufferSizeForBluetoothRoute(buffer_size_in_frames, sample_rate);
        if (clamped != buffer_size_in_frames) {
            thl::Logger::logf(thl::Logger::LogLevel::Warning,
                              "thl.audio_io.audio_device_manager",
                              "%s: Bluetooth route active — clamping buffer from %u to %u frames "
                              "(max %.0f ms)",
                              label,
                              buffer_size_in_frames,
                              clamped,
                              kMaxBluetoothIOBufferDurationSeconds * 1000.0f);
            buffer_size_in_frames = clamped;
        }
    }
#endif

#if defined(THL_PLATFORM_ANDROID)
    {
        auto probe = configureAndroidBuffering(m_impl->m_context, dev_config, buffer_size_in_frames);

        burst_size = probe.burstSize;
        dev_config.periodSizeInFrames = probe.periodSize;
        configured_periods = probe.periods;
        dev_config.periods = probe.periods;

        thl::Logger::logf(thl::Logger::LogLevel::Debug,
                          "thl.audio_io.audio_device_manager",
                          "%s: probed burstSize=%u, configured periodSize=%u, periods=%u",
                          label,
                          burst_size,
                          probe.periodSize,
                          probe.periods);
    }
#elif defined(THL_PLATFORM_IOS) || defined(THL_PLATFORM_MACOS)
    {
        auto probe = probeCoreAudioPeriod(m_impl->m_context, dev_config, buffer_size_in_frames);

        if (probe.periodSize > 0 && probe.periodSize != buffer_size_in_frames) {
            thl::Logger::logf(thl::Logger::LogLevel::Warning,
                              "thl.audio_io.audio_device_manager",
                              "%s: probed periodSize=%u differs from requested %u, adjusting",
                              label,
                              probe.periodSize,
                              buffer_size_in_frames);
        }

        burst_size = probe.periodSize;
        dev_config.periodSizeInFrames = probe.periodSize;
        configured_periods = (probe.periods > 0) ? probe.periods : 1;
        dev_config.periods = configured_periods;

        thl::Logger::logf(thl::Logger::LogLevel::Debug,
                          "thl.audio_io.audio_device_manager",
                          "%s: probed periodSize=%u, periods=%u",
                          label,
                          probe.periodSize,
                          configured_periods);
    }
#else
    dev_config.periodSizeInFrames = buffer_size_in_frames;
#endif

    // --- Open device ---
    if (ma_device_init(&m_impl->m_context, &dev_config, device) != MA_SUCCESS) { return false; }

    // --- Resolve actual period size ---
    uint32_t resolved_period_size = 0;
#if defined(THL_PLATFORM_ANDROID)
    resolved_period_size = resolveAAudioPeriodSize(m_impl->m_context, *device, device_type);
    if (resolved_period_size == 0) { resolved_period_size = dev_config.periodSizeInFrames; }
#else
    if (role == DeviceRole::Capture) {
        resolved_period_size = device->capture.internalPeriodSizeInFrames;
    } else {
        resolved_period_size = device->playback.internalPeriodSizeInFrames;
    }
    if (resolved_period_size == 0) { resolved_period_size = buffer_size_in_frames; }
#endif

    thl::Logger::logf(thl::Logger::LogLevel::Debug,
                      "thl.audio_io.audio_device_manager",
                      "%s: initialised device '%s', sampleRate=%u, periodSize=%u, periods=%u, "
                      "channels(out=%u, in=%u)",
                      label,
                      (role == DeviceRole::Playback || role == DeviceRole::Duplex)
                          ? output_device->m_name.c_str()
                          : input_device->m_name.c_str(),
                      sample_rate,
                      resolved_period_size,
                      configured_periods,
                      num_output_channels,
                      num_input_channels);

    // --- Store results ---
    switch (role) {
        case DeviceRole::Playback:
            m_impl->m_playback_device_initialised = true;
            m_num_output_channels = device->playback.channels;
            m_sample_rate = device->sampleRate;
            m_impl->m_playback_prepared_period_size = resolved_period_size;
            m_impl->m_playback_periods = configured_periods;
            m_impl->m_playback_burst_size = burst_size;
            break;
        case DeviceRole::Capture:
            m_impl->m_capture_device_initialised = true;
            m_num_input_channels = device->capture.channels;
            if (!m_impl->m_playback_device_initialised) { m_sample_rate = device->sampleRate; }
            m_impl->m_capture_prepared_period_size = resolved_period_size;
            m_impl->m_capture_periods = configured_periods;
            m_impl->m_capture_burst_size = burst_size;
            break;
        case DeviceRole::Duplex:
            m_impl->m_duplex_device_initialised = true;
            if (!m_impl->m_playback_device_initialised && !m_impl->m_capture_device_initialised) {
                m_sample_rate = device->sampleRate;
            }
            m_impl->m_duplex_prepared_period_size = resolved_period_size;
            m_impl->m_duplex_periods = configured_periods;
            m_impl->m_duplex_burst_size = burst_size;
            break;
    }

    return true;
}

bool AudioDeviceManager::initialise(const AudioDeviceInfo* input_device,
                                    const AudioDeviceInfo* output_device,
                                    uint32_t sample_rate,
                                    uint32_t buffer_size_in_frames,
                                    uint32_t num_input_channels,
                                    uint32_t num_output_channels) {
    if (!m_impl->m_context_initialised) { return false; }

    thl::Logger::logf(thl::Logger::LogLevel::Info,
                      "thl.audio_io.audio_device_manager",
                      "Initialising AudioDeviceManager with sampleRate=%u, bufferSizeInFrames=%u, "
                      "numInputChannels=%u, numOutputChannels=%u",
                      sample_rate,
                      buffer_size_in_frames,
                      num_input_channels,
                      num_output_channels);

    shutdown();

    m_sample_rate = sample_rate;
    m_num_input_channels = input_device ? num_input_channels : 0;
    m_num_output_channels = output_device ? num_output_channels : 0;
    m_impl->m_output_device_name = output_device ? output_device->m_name : "";
    m_impl->m_input_device_name = input_device ? input_device->m_name : "";
    m_impl->m_playback_prepared_period_size = buffer_size_in_frames;
    m_impl->m_capture_prepared_period_size = buffer_size_in_frames;
    m_impl->m_duplex_prepared_period_size = buffer_size_in_frames;
    m_impl->m_playback_periods = 1;
    m_impl->m_capture_periods = 1;
    m_impl->m_duplex_periods = 1;
    m_impl->m_playback_burst_size = 0;
    m_impl->m_capture_burst_size = 0;
    m_impl->m_duplex_burst_size = 0;

    bool any_initialised = false;
    any_initialised |= try_initialise_device(DeviceRole::Playback,
                                          input_device,
                                          output_device,
                                          sample_rate,
                                          buffer_size_in_frames,
                                          num_input_channels,
                                          num_output_channels);
    any_initialised |= try_initialise_device(DeviceRole::Capture,
                                          input_device,
                                          output_device,
                                          sample_rate,
                                          buffer_size_in_frames,
                                          num_input_channels,
                                          num_output_channels);
    any_initialised |= try_initialise_device(DeviceRole::Duplex,
                                          input_device,
                                          output_device,
                                          sample_rate,
                                          buffer_size_in_frames,
                                          num_input_channels,
                                          num_output_channels);

    if (!any_initialised) { return false; }

    auto prepare_callbacks =
        [](RCU<std::vector<AudioIODeviceCallback*>>& list, ma_device& device, uint32_t buffer_size) {
            list.read([&](const auto& callbacks) {
                for (auto* callback : callbacks) {
                    callback->prepare_to_play(device.sampleRate, buffer_size);
                }
            });
        };

    if (m_impl->m_playback_device_initialised) {
        prepare_callbacks(m_playback_callbacks,
                         m_impl->m_playback_device,
                         m_impl->m_playback_prepared_period_size);
    }
    if (m_impl->m_capture_device_initialised) {
        prepare_callbacks(m_capture_callbacks,
                         m_impl->m_capture_device,
                         m_impl->m_capture_prepared_period_size);
    }
    if (m_impl->m_duplex_device_initialised) {
        prepare_callbacks(m_duplex_callbacks, m_impl->m_duplex_device, m_impl->m_duplex_prepared_period_size);
    }

    return true;
}

void AudioDeviceManager::shutdown() {
    stop_playback();
    stop_capture();
    stop_duplex();

#if defined(THL_PLATFORM_ANDROID)
    // On API <= 28, AAudio wraps AudioTrack internally and
    // ma_device_uninit() can deadlock in ~AudioTrack::requestExitAndWait().
    // Use timed async uninit on the legacy path; sync uninit on API 29+.
    const int androidApi = getAndroidApiLevel();

    if (androidApi <= 28) {
        thl::Logger::logf(thl::Logger::LogLevel::Info,
                          "thl.audio_io.audio_device_manager",
                          "shutdown: API %d — using async timed uninit (legacy AudioTrack path)",
                          androidApi);
        // Give AudioTrack callback threads time to notice the stop and exit. This is where the
        // deadlock can occur if we uninit too early, as android is in the process of tearing down
        // the AudioTrack but hasn't completed yet. 100 ms is somewhat arbitrary but should be
        // sufficient for even slow devices to exit their callbacks. To make sure we do not block
        // the shutdown thread at any cost we also do async uninit with a timeout below, but in the
        // common case where callbacks exit promptly this sleep should allow the uninit to complete
        // without needing to spawn a worker thread at all.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        auto timedUninit = [](ma_device& device, const char* label) {
            auto* heap = static_cast<ma_device*>(std::malloc(sizeof(ma_device)));
            if (!heap) {
                thl::Logger::logf(thl::Logger::LogLevel::Warning,
                                  "thl.audio_io.audio_device_manager",
                                  "%s: malloc failed, falling back to sync uninit",
                                  label);
                ma_device_uninit(&device);
                return;
            }
            std::memcpy(heap, &device, sizeof(ma_device));
            std::memset(&device, 0, sizeof(ma_device));

            auto done = std::make_shared<std::atomic<bool>>(false);

            std::thread worker([heap, label, done]() {
                ma_device_uninit(heap);
                std::free(heap);
                done->store(true, std::memory_order_release);
                thl::Logger::logf(thl::Logger::LogLevel::Info,
                                  "thl.audio_io.audio_device_manager",
                                  "%s: device cleanup completed",
                                  label);
            });

            constexpr auto kTimeout = std::chrono::milliseconds(300);
            auto deadline = std::chrono::steady_clock::now() + kTimeout;
            while (!done->load(std::memory_order_acquire)) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    worker.detach();
                    thl::Logger::logf(thl::Logger::LogLevel::Warning,
                                      "thl.audio_io.audio_device_manager",
                                      "%s: uninit timed out, detached worker",
                                      label);
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            worker.join();
        };

        if (m_impl->playbackDeviceInitialised) {
            timedUninit(m_impl->playbackDevice, "playback");
            m_impl->playbackDeviceInitialised = false;
            m_impl->playbackActualPeriodSize.store(0, std::memory_order_relaxed);
        }
        if (m_impl->captureDeviceInitialised) {
            timedUninit(m_impl->captureDevice, "capture");
            m_impl->captureDeviceInitialised = false;
            m_impl->captureActualPeriodSize.store(0, std::memory_order_relaxed);
        }
        if (m_impl->duplexDeviceInitialised) {
            timedUninit(m_impl->duplexDevice, "duplex");
            m_impl->duplexDeviceInitialised = false;
            m_impl->duplexActualPeriodSize.store(0, std::memory_order_relaxed);
        }
    } else {
        // API 29+: native AAudio, no AudioTrack deadlock risk.
        thl::Logger::logf(thl::Logger::LogLevel::Info,
                          "thl.audio_io.audio_device_manager",
                          "shutdown: API %d — using sync uninit (native AAudio path)",
                          androidApi);
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
#else
    if (m_impl->m_playback_device_initialised) {
        ma_device_uninit(&m_impl->m_playback_device);
        m_impl->m_playback_device_initialised = false;
        m_impl->m_playback_actual_period_size.store(0, std::memory_order_relaxed);
    }
    if (m_impl->m_capture_device_initialised) {
        ma_device_uninit(&m_impl->m_capture_device);
        m_impl->m_capture_device_initialised = false;
        m_impl->m_capture_actual_period_size.store(0, std::memory_order_relaxed);
    }
    if (m_impl->m_duplex_device_initialised) {
        ma_device_uninit(&m_impl->m_duplex_device);
        m_impl->m_duplex_device_initialised = false;
        m_impl->m_duplex_actual_period_size.store(0, std::memory_order_relaxed);
    }
#endif

    m_impl->m_output_device_name.clear();
    m_impl->m_input_device_name.clear();
}

bool AudioDeviceManager::start_playback() {
    if (!m_impl->m_playback_device_initialised || m_playback_running.load(std::memory_order_relaxed)) {
        return false;
    }

    ma_result result = ma_device_start(&m_impl->m_playback_device);
    if (result != MA_SUCCESS) { return false; }

    m_playback_running.store(true, std::memory_order_relaxed);
    return true;
}

void AudioDeviceManager::stop_playback() {
    if (!m_playback_running.load(std::memory_order_relaxed)) { return; }

    m_playback_running.store(false, std::memory_order_relaxed);
    ma_device_stop(&m_impl->m_playback_device);
    m_playback_audio_thread_registered.store(false, std::memory_order_relaxed);

    m_playback_callbacks.read([](const auto& callbacks) {
        for (auto* callback : callbacks) { callback->release_resources(); }
    });
}

bool AudioDeviceManager::start_capture() {
    if (!m_impl->m_capture_device_initialised || m_capture_running.load(std::memory_order_relaxed)) {
        return false;
    }

    // Reset rate measurement so a fresh calibration runs for this session.
    m_impl->m_capture_first_callback_us.store(0, std::memory_order_relaxed);
    m_impl->m_capture_total_frames.store(0, std::memory_order_relaxed);
    m_impl->m_capture_measured_rate.store(0, std::memory_order_relaxed);

    ma_result result = ma_device_start(&m_impl->m_capture_device);
    if (result != MA_SUCCESS) { return false; }

    m_capture_running.store(true, std::memory_order_relaxed);
    return true;
}

void AudioDeviceManager::stop_capture() {
    if (!m_capture_running.load(std::memory_order_relaxed)) { return; }

    m_capture_running.store(false, std::memory_order_relaxed);
    ma_device_stop(&m_impl->m_capture_device);
    m_capture_audio_thread_registered.store(false, std::memory_order_relaxed);

    m_capture_callbacks.read([](const auto& callbacks) {
        for (auto* callback : callbacks) { callback->release_resources(); }
    });
}

bool AudioDeviceManager::start_duplex() {
    if (!m_impl->m_duplex_device_initialised || m_duplex_running.load(std::memory_order_relaxed)) {
        return false;
    }

    ma_result result = ma_device_start(&m_impl->m_duplex_device);
    if (result != MA_SUCCESS) { return false; }

    m_duplex_running.store(true, std::memory_order_relaxed);
    return true;
}

void AudioDeviceManager::stop_duplex() {
    if (!m_duplex_running.load(std::memory_order_relaxed)) { return; }

    m_duplex_running.store(false, std::memory_order_relaxed);
    ma_device_stop(&m_impl->m_duplex_device);
    m_duplex_audio_thread_registered.store(false, std::memory_order_relaxed);

    m_duplex_callbacks.read([](const auto& callbacks) {
        for (auto* callback : callbacks) { callback->release_resources(); }
    });
}

void AudioDeviceManager::add_playback_callback(AudioIODeviceCallback* callback) {
    if (!callback) { return; }

    bool added = false;
    m_playback_callbacks.update([&](auto& callbacks) {
        if (std::find(callbacks.begin(), callbacks.end(), callback) == callbacks.end()) {
            callbacks.push_back(callback);
            added = true;
        }
    });

    if (added && m_impl->m_playback_device_initialised) {
        callback->prepare_to_play(m_impl->m_playback_device.sampleRate,
                                m_impl->m_playback_prepared_period_size);
    }
}

void AudioDeviceManager::add_capture_callback(AudioIODeviceCallback* callback) {
    if (!callback) { return; }

    bool added = false;
    m_capture_callbacks.update([&](auto& callbacks) {
        if (std::find(callbacks.begin(), callbacks.end(), callback) == callbacks.end()) {
            callbacks.push_back(callback);
            added = true;
        }
    });

    if (added && m_impl->m_capture_device_initialised) {
        callback->prepare_to_play(m_impl->m_capture_device.sampleRate,
                                m_impl->m_capture_prepared_period_size);
    }
}

void AudioDeviceManager::add_duplex_callback(AudioIODeviceCallback* callback) {
    if (!callback) { return; }

    bool added = false;
    m_duplex_callbacks.update([&](auto& callbacks) {
        if (std::find(callbacks.begin(), callbacks.end(), callback) == callbacks.end()) {
            callbacks.push_back(callback);
            added = true;
        }
    });

    if (added && m_impl->m_duplex_device_initialised) {
        callback->prepare_to_play(m_impl->m_duplex_device.sampleRate, m_impl->m_duplex_prepared_period_size);
    }
}

void AudioDeviceManager::remove_playback_callback(AudioIODeviceCallback* callback) {
    AudioIODeviceCallback* removed = nullptr;
    m_playback_callbacks.update([&](auto& callbacks) {
        auto it = std::find(callbacks.begin(), callbacks.end(), callback);
        if (it != callbacks.end()) {
            removed = *it;
            callbacks.erase(it);
        }
    });

    if (removed) { removed->release_resources(); }
}

void AudioDeviceManager::remove_capture_callback(AudioIODeviceCallback* callback) {
    AudioIODeviceCallback* removed = nullptr;
    m_capture_callbacks.update([&](auto& callbacks) {
        auto it = std::find(callbacks.begin(), callbacks.end(), callback);
        if (it != callbacks.end()) {
            removed = *it;
            callbacks.erase(it);
        }
    });

    if (removed) { removed->release_resources(); }
}

void AudioDeviceManager::remove_duplex_callback(AudioIODeviceCallback* callback) {
    AudioIODeviceCallback* removed = nullptr;
    m_duplex_callbacks.update([&](auto& callbacks) {
        auto it = std::find(callbacks.begin(), callbacks.end(), callback);
        if (it != callbacks.end()) {
            removed = *it;
            callbacks.erase(it);
        }
    });

    if (removed) { removed->release_resources(); }
}

void AudioDeviceManager::set_device_notification_callback(DeviceNotificationCallback callback) {
    auto ptr =
        callback ? std::make_shared<DeviceNotificationCallback>(std::move(callback)) : nullptr;
    std::atomic_store_explicit(&m_notification_callback, std::move(ptr), std::memory_order_release);
}

void AudioDeviceManager::set_log_callback(LogCallback callback) {
    m_impl->m_log_callback = std::move(callback);

    if (!m_impl->m_context_initialised) { return; }

    ma_log* log = ma_context_get_log(&m_impl->m_context);
    if (!log) { return; }

    if (m_impl->m_log_callback) {
        ma_log_register_callback(log, ma_log_callback_init(static_log_callback, m_impl.get()));
    } else {
        ma_log_unregister_callback(log, ma_log_callback_init(static_log_callback, m_impl.get()));
    }
}

const char* bluetooth_profile_to_string(BluetoothProfile p) {
    switch (p) {
        case BluetoothProfile::HFP: return "HFP";
        case BluetoothProfile::A2DP: return "A2DP";
    }
    return "A2DP";
}

BluetoothProfile bluetooth_profile_from_string(const std::string& str) {
    if (str == "HFP") { return BluetoothProfile::HFP; }
    return BluetoothProfile::A2DP;
}

std::vector<BluetoothProfile> get_supported_bluetooth_profiles() {
    std::vector<BluetoothProfile> profiles;
    profiles.push_back(BluetoothProfile::A2DP);
#if defined(THL_PLATFORM_ANDROID)
    if (getAndroidApiLevel() > 30) { profiles.push_back(BluetoothProfile::HFP); }
#else
    profiles.push_back(BluetoothProfile::HFP);
#endif
    return profiles;
}

bool is_classic_bluetooth_connected() {
#if defined(THL_PLATFORM_IOS)
    return isIOSClassicBluetoothConnected();
#elif defined(THL_PLATFORM_ANDROID)
    return isAndroidClassicBluetoothConnected();
#else
    return false;
#endif
}

bool AudioDeviceManager::set_bluetooth_profile(BluetoothProfile profile) {
#if defined(THL_PLATFORM_IOS)
    if (!setIOSBluetoothProfile(profile, nullptr)) { return false; }
#elif defined(THL_PLATFORM_ANDROID)
    // On Android, SCO requires setCommunicationDevice() which is only
    // available on API 31+.  On older versions only A2DP is supported.
    if (getAndroidApiLevel() <= 30) {
        if (profile != BluetoothProfile::A2DP) {
            thl::Logger::logf(thl::Logger::LogLevel::Warning,
                              "thl.audio_io.audio_device_manager",
                              "Bluetooth profile '%s' not supported on API %d (requires API 31+), "
                              "profile remains %s",
                              bluetoothProfileToString(profile),
                              getAndroidApiLevel(),
                              bluetoothProfileToString(m_bluetoothProfile));
            return false;
        }

        // A2DP is the default media route on API <= 30.
        // Ensure SCO is disabled and treat this as success.
        if (isAndroidBluetoothScoEnabled()) { setAndroidBluetoothSco(false); }
        m_bluetoothProfile = BluetoothProfile::A2DP;
        return true;
    }
    {
        bool wantSco = (profile == BluetoothProfile::HFP);
        bool scoNow = isAndroidBluetoothScoEnabled();

        if (wantSco) {
            // Always cycle the SCO link when requesting HFP. After a
            // Bluetooth reconnection the old SCO session is dead but
            // g_scoEnabled may still be true — skipping the restart
            // leaves the audio route on the internal mic / A2DP.
            if (scoNow) { setAndroidBluetoothSco(false); }
            setAndroidBluetoothSco(true);
        } else if (scoNow) {
            setAndroidBluetoothSco(false);
        }

        thl::Logger::logf(thl::Logger::LogLevel::Info,
                          "thl.audio_io.audio_device_manager",
                          "Bluetooth profile set to %s (SCO %s)",
                          wantSco ? "HFP" : "A2DP",
                          wantSco ? "started" : "stopped");
    }
#else
    (void)profile;
#endif

    m_bluetooth_profile = profile;
    return true;
}

BluetoothProfile AudioDeviceManager::get_bluetooth_profile() const {
    return m_bluetooth_profile;
}

uint32_t AudioDeviceManager::get_sample_rate() const {
    if (m_impl->m_playback_device_initialised) { return m_impl->m_playback_device.sampleRate; }
    if (m_impl->m_duplex_device_initialised) { return m_impl->m_duplex_device.sampleRate; }
    if (m_impl->m_capture_device_initialised) { return m_impl->m_capture_device.sampleRate; }
    return m_sample_rate;
}

uint32_t AudioDeviceManager::get_capture_sample_rate() const {
#if defined(THL_PLATFORM_ANDROID)
    // AAudio lies about the SCO capture sample rate on some devices (reports
    // 48 kHz when the codec actually delivers 8/16 kHz) but on others the
    // HAL genuinely resamples to 48 kHz.  Use the measured delivery rate
    // from the capture callback when available — it reflects the actual
    // frame rate regardless of what the API reports.
    if (isAndroidBluetoothScoEnabled() &&
        getCurrentInputDeviceName().find("Bluetooth SCO") != std::string::npos) {
        uint32_t measured = m_impl->captureMeasuredRate.load(std::memory_order_relaxed);
        if (measured > 0) {
            thl::Logger::logf(thl::Logger::LogLevel::Info,
                              "thl.audio_io.audio_device_manager",
                              "Using measured SCO capture rate: %u",
                              measured);
            return measured;
        }
        // Measurement not ready yet — fall back to API-reported rate.
        // Callers should use waitForCaptureRateMeasurement() first when
        // accuracy is critical (e.g. before opening a WAV file).
        thl::Logger::logf(thl::Logger::LogLevel::Warning,
                          "thl.audio_io.audio_device_manager",
                          "SCO capture rate measurement not ready, "
                          "falling back to API-reported rate");
    }
#endif
    return get_sample_rate();
}

bool AudioDeviceManager::wait_for_capture_rate_measurement(uint32_t timeout_ms) const {
#if defined(THL_PLATFORM_ANDROID)
    if (!isAndroidBluetoothScoEnabled() || !m_captureRunning.load(std::memory_order_relaxed) ||
        getCurrentInputDeviceName().find("Bluetooth SCO") == std::string::npos) {
        return false;
    }

    constexpr uint32_t kPollIntervalMs = 50;
    uint32_t elapsed = 0;
    while (elapsed < timeoutMs) {
        if (m_impl->captureMeasuredRate.load(std::memory_order_relaxed) > 0) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
        elapsed += kPollIntervalMs;
    }
    thl::Logger::logf(thl::Logger::LogLevel::Warning,
                      "thl.audio_io.audio_device_manager",
                      "Capture rate measurement timed out after %u ms",
                      timeoutMs);
    return false;
#else
    (void)timeout_ms;
    return false;
#endif
}

uint32_t AudioDeviceManager::get_buffer_size() const {
#if defined(THL_PLATFORM_ANDROID)
    if (m_impl->playbackDeviceInitialised) {
        return m_impl->playbackPreparedPeriodSize * m_impl->playbackPeriods;
    }
    if (m_impl->duplexDeviceInitialised) {
        return m_impl->duplexPreparedPeriodSize * m_impl->duplexPeriods;
    }
    if (m_impl->captureDeviceInitialised) {
        return m_impl->capturePreparedPeriodSize * m_impl->capturePeriods;
    }
#else
    if (m_impl->m_playback_device_initialised) { return m_impl->m_playback_prepared_period_size; }
    if (m_impl->m_duplex_device_initialised) { return m_impl->m_duplex_prepared_period_size; }
    if (m_impl->m_capture_device_initialised) { return m_impl->m_capture_prepared_period_size; }
#endif
    return 0;
}

uint32_t AudioDeviceManager::get_buffer_size(DeviceRole role) const {
    switch (role) {
        case DeviceRole::Playback:
            if (m_impl->m_playback_device_initialised)
#if defined(THL_PLATFORM_ANDROID)
                return m_impl->playbackPreparedPeriodSize * m_impl->playbackPeriods;
#else
                return m_impl->m_playback_prepared_period_size;
#endif
            break;
        case DeviceRole::Capture:
            if (m_impl->m_capture_device_initialised)
#if defined(THL_PLATFORM_ANDROID)
                return m_impl->capturePreparedPeriodSize * m_impl->capturePeriods;
#else
                return m_impl->m_capture_prepared_period_size;
#endif
            break;
        case DeviceRole::Duplex:
            if (m_impl->m_duplex_device_initialised)
#if defined(THL_PLATFORM_ANDROID)
                return m_impl->duplexPreparedPeriodSize * m_impl->duplexPeriods;
#else
                return m_impl->m_duplex_prepared_period_size;
#endif
            break;
    }
    return get_buffer_size();
}

uint32_t AudioDeviceManager::get_period_size() const {
    // Prefer the actual callback frame count resolved from the first callback.
    if (m_impl->m_playback_device_initialised) {
        uint32_t actual = m_impl->m_playback_actual_period_size.load(std::memory_order_relaxed);
        return actual > 0 ? actual : m_impl->m_playback_prepared_period_size;
    }
    if (m_impl->m_duplex_device_initialised) {
        uint32_t actual = m_impl->m_duplex_actual_period_size.load(std::memory_order_relaxed);
        return actual > 0 ? actual : m_impl->m_duplex_prepared_period_size;
    }
    if (m_impl->m_capture_device_initialised) {
        uint32_t actual = m_impl->m_capture_actual_period_size.load(std::memory_order_relaxed);
        return actual > 0 ? actual : m_impl->m_capture_prepared_period_size;
    }
    return get_buffer_size();
}

uint32_t AudioDeviceManager::get_period_size(DeviceRole role) const {
    switch (role) {
        case DeviceRole::Playback:
            if (m_impl->m_playback_device_initialised) {
                uint32_t actual = m_impl->m_playback_actual_period_size.load(std::memory_order_relaxed);
                return actual > 0 ? actual : m_impl->m_playback_prepared_period_size;
            }
            break;
        case DeviceRole::Capture:
            if (m_impl->m_capture_device_initialised) {
                uint32_t actual = m_impl->m_capture_actual_period_size.load(std::memory_order_relaxed);
                return actual > 0 ? actual : m_impl->m_capture_prepared_period_size;
            }
            break;
        case DeviceRole::Duplex:
            if (m_impl->m_duplex_device_initialised) {
                uint32_t actual = m_impl->m_duplex_actual_period_size.load(std::memory_order_relaxed);
                return actual > 0 ? actual : m_impl->m_duplex_prepared_period_size;
            }
            break;
    }
    return get_period_size();
}

uint32_t AudioDeviceManager::get_period_count() const {
    if (m_impl->m_playback_device_initialised) { return m_impl->m_playback_periods; }
    if (m_impl->m_duplex_device_initialised) { return m_impl->m_duplex_periods; }
    if (m_impl->m_capture_device_initialised) { return m_impl->m_capture_periods; }
    return 1;
}

uint32_t AudioDeviceManager::get_period_count(DeviceRole role) const {
    switch (role) {
        case DeviceRole::Playback:
            if (m_impl->m_playback_device_initialised) { return m_impl->m_playback_periods; }
            break;
        case DeviceRole::Capture:
            if (m_impl->m_capture_device_initialised) { return m_impl->m_capture_periods; }
            break;
        case DeviceRole::Duplex:
            if (m_impl->m_duplex_device_initialised) { return m_impl->m_duplex_periods; }
            break;
    }
    return 1;
}

uint32_t AudioDeviceManager::get_burst_size() const {
    if (m_impl->m_playback_device_initialised && m_impl->m_playback_burst_size > 0) {
        return m_impl->m_playback_burst_size;
    }
    if (m_impl->m_duplex_device_initialised && m_impl->m_duplex_burst_size > 0) {
        return m_impl->m_duplex_burst_size;
    }
    if (m_impl->m_capture_device_initialised && m_impl->m_capture_burst_size > 0) {
        return m_impl->m_capture_burst_size;
    }
    return get_period_size();
}

uint32_t AudioDeviceManager::get_burst_size(DeviceRole role) const {
    switch (role) {
        case DeviceRole::Playback:
            if (m_impl->m_playback_device_initialised && m_impl->m_playback_burst_size > 0) {
                return m_impl->m_playback_burst_size;
            }
            break;
        case DeviceRole::Capture:
            if (m_impl->m_capture_device_initialised && m_impl->m_capture_burst_size > 0) {
                return m_impl->m_capture_burst_size;
            }
            break;
        case DeviceRole::Duplex:
            if (m_impl->m_duplex_device_initialised && m_impl->m_duplex_burst_size > 0) {
                return m_impl->m_duplex_burst_size;
            }
            break;
    }
    return get_period_size(role);
}

uint32_t AudioDeviceManager::get_num_input_channels() const {
    if (m_impl->m_capture_device_initialised) { return m_impl->m_capture_device.capture.channels; }
    if (m_impl->m_duplex_device_initialised) { return m_impl->m_duplex_device.capture.channels; }
    return m_num_input_channels;
}

uint32_t AudioDeviceManager::get_num_output_channels() const {
    if (m_impl->m_playback_device_initialised) { return m_impl->m_playback_device.playback.channels; }
    if (m_impl->m_duplex_device_initialised) { return m_impl->m_duplex_device.playback.channels; }
    return m_num_output_channels;
}

std::string AudioDeviceManager::get_current_output_device_name() const {
#if defined(THL_PLATFORM_IOS)
    return getIOSCurrentOutputRouteName();
#elif defined(THL_PLATFORM_ANDROID)
    {
        int32_t deviceId = 0;
        if (m_impl->playbackDeviceInitialised) {
            deviceId = m_impl->playbackDevice.playback.id.aaudio;
        } else if (m_impl->duplexDeviceInitialised) {
            deviceId = m_impl->duplexDevice.playback.id.aaudio;
        }
        return getAndroidActiveOutputDeviceName(deviceId);
    }
#else
    if (m_impl->m_playback_device_initialised) { return m_impl->m_playback_device.playback.name; }
    if (m_impl->m_duplex_device_initialised) { return m_impl->m_duplex_device.playback.name; }
    return {};
#endif
}

std::string AudioDeviceManager::get_current_input_device_name() const {
#if defined(THL_PLATFORM_IOS)
    return getIOSCurrentInputRouteName();
#elif defined(THL_PLATFORM_ANDROID)
    {
        int32_t deviceId = 0;
        if (m_impl->captureDeviceInitialised) {
            deviceId = m_impl->captureDevice.capture.id.aaudio;
        } else if (m_impl->duplexDeviceInitialised) {
            deviceId = m_impl->duplexDevice.capture.id.aaudio;
        }
        return getAndroidActiveInputDeviceName(deviceId);
    }
#else
    if (m_impl->m_capture_device_initialised) { return m_impl->m_capture_device.capture.name; }
    if (m_impl->m_duplex_device_initialised) { return m_impl->m_duplex_device.capture.name; }
    return {};
#endif
}

void AudioDeviceManager::process_callbacks(DeviceRole role,
                                          void* device_ptr,
                                          float* output,
                                          const float* input,
                                          uint32_t frame_count) {
    auto* device = static_cast<ma_device*>(device_ptr);
    if (!device) { return; }

    // Select per-role state.
    RCU<std::vector<AudioIODeviceCallback*>>* callbacks = nullptr;
    std::atomic<bool>* audio_thread_registered = nullptr;
    uint32_t max_chunk_size = 0;
    std::atomic<uint32_t>* actual_period_size = nullptr;

    switch (role) {
        case DeviceRole::Playback:
            callbacks = &m_playback_callbacks;
            audio_thread_registered = &m_playback_audio_thread_registered;
            max_chunk_size = m_impl->m_playback_prepared_period_size;
            actual_period_size = &m_impl->m_playback_actual_period_size;
            break;
        case DeviceRole::Capture:
            callbacks = &m_capture_callbacks;
            audio_thread_registered = &m_capture_audio_thread_registered;
            max_chunk_size = m_impl->m_capture_prepared_period_size;
            actual_period_size = &m_impl->m_capture_actual_period_size;
            break;
        case DeviceRole::Duplex:
            callbacks = &m_duplex_callbacks;
            audio_thread_registered = &m_duplex_audio_thread_registered;
            max_chunk_size = m_impl->m_duplex_prepared_period_size;
            actual_period_size = &m_impl->m_duplex_actual_period_size;
            break;
    }

    if (!callbacks || !audio_thread_registered) { return; }

    // Record the true period size from the first callback for reporting via
    // getPeriodSize(). On iOS the actual AU render callback may deliver fewer
    // frames than internalPeriodSizeInFrames / AVAudioSession report.
    if (actual_period_size->load(std::memory_order_relaxed) == 0) {
        actual_period_size->store(frame_count, std::memory_order_relaxed);
        if (frame_count != max_chunk_size) {
            thl::Logger::logf(
                thl::Logger::LogLevel::Warning,
                "thl.audio_io.audio_device_manager",
                "Actual first callback frameCount %u differs from prepared periodSize %u "
                "(role %d)",
                frame_count,
                max_chunk_size,
                static_cast<int>(role));
        }
    }

#if defined(THL_PLATFORM_ANDROID)
    // Measure actual capture delivery rate by timing callbacks.
    // Only needed for SCO devices where Android lies about the rate.
    if (role == DeviceRole::Capture && isAndroidBluetoothScoEnabled() &&
        getCurrentInputDeviceName().find("Bluetooth SCO") != std::string::npos &&
        m_impl->captureMeasuredRate.load(std::memory_order_relaxed) == 0) {
        auto nowUs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                               std::chrono::steady_clock::now().time_since_epoch())
                                               .count());

        uint64_t firstUs = m_impl->captureFirstCallbackUs.load(std::memory_order_relaxed);
        if (firstUs == 0) {
            m_impl->captureFirstCallbackUs.store(nowUs, std::memory_order_relaxed);
            m_impl->captureTotalFrames.store(frameCount, std::memory_order_relaxed);
        } else {
            uint64_t total =
                m_impl->captureTotalFrames.load(std::memory_order_relaxed) + frameCount;
            m_impl->captureTotalFrames.store(total, std::memory_order_relaxed);

            if (total >= Impl::kRateCalibrationFrames) {
                uint64_t elapsedUs = nowUs - firstUs;
                if (elapsedUs > 0) {
                    uint32_t measured =
                        static_cast<uint32_t>((total * 1000000ULL + elapsedUs / 2) / elapsedUs);
                    // Snap to nearest standard rate
                    constexpr uint32_t kStdRates[] = {8000, 16000, 32000, 48000};
                    uint32_t best = measured;
                    uint32_t bestDist = UINT32_MAX;
                    for (uint32_t sr : kStdRates) {
                        uint32_t dist = measured > sr ? measured - sr : sr - measured;
                        if (dist < bestDist) {
                            bestDist = dist;
                            best = sr;
                        }
                    }
                    m_impl->captureMeasuredRate.store(best, std::memory_order_relaxed);
                    thl::Logger::logf(thl::Logger::LogLevel::Info,
                                      "thl.audio_io.audio_device_manager",
                                      "Capture rate measurement: %u frames in %llu us "
                                      "= %u Hz (snapped to %u Hz)",
                                      static_cast<uint32_t>(total),
                                      elapsedUs,
                                      measured,
                                      best);
                }
            }
        }
    }
#endif

    // One-time RCU reader registration for this audio thread.
    if (!audio_thread_registered->load(std::memory_order_relaxed)) [[unlikely]] {
        callbacks->register_reader_thread();
        audio_thread_registered->store(true, std::memory_order_relaxed);
    }

    uint32_t output_channels = device->playback.channels;
    uint32_t input_channels = device->capture.channels;
    if (output_channels == 0 && input_channels == 0) { return; }

#if defined(THL_DEBUG) && !defined(THL_PLATFORM_IOS_SIMULATOR)
    // {
    //     static uint32_t callbackCounter = 0;
    //     if (frameCount != actualPeriodSize->load(std::memory_order_relaxed)) {
    //         thl::Logger::logf(thl::Logger::LogLevel::Warning,
    //                            "thl.audio_io.audio_device_manager",
    //                            "Callback #%u: frameCount %u, prepared %u, previous %u (role %d)",
    //                            callbackCounter, frameCount, maxChunkSize,
    //                            actualPeriodSize->load(std::memory_order_relaxed),
    //                            static_cast<int>(role));
    //         actualPeriodSize->store(frameCount, std::memory_order_relaxed);
    //     }
    //     if (callbackCounter % 100 == 0) {
    //         thl::Logger::logf(thl::Logger::LogLevel::Debug,
    //                            "thl.audio_io.audio_device_manager",
    //                            "Callback #%u: frameCount %u, prepared %u, previous %u (role %d)",
    //                            callbackCounter, frameCount, maxChunkSize,
    //                            actualPeriodSize->load(std::memory_order_relaxed),
    //                            static_cast<int>(role));
    //     }
    //     ++callbackCounter;
    // }
#endif

    // Zero output so callbacks can additively mix.
    if (output && output_channels > 0) {
        std::memset(output, 0, frame_count * output_channels * sizeof(float));
    }

    float* safe_output = output_channels > 0 ? output : nullptr;
    const float* safe_input = input_channels > 0 ? input : nullptr;

    // Process in chunks no larger than preparedPeriodSize to avoid overflowing
    // internal DSP buffers allocated by prepare_to_play().
    uint32_t frames_remaining = frame_count;
    uint32_t offset = 0;

    while (frames_remaining > 0) {
        uint32_t chunk =
            (max_chunk_size > 0) ? std::min(frames_remaining, max_chunk_size) : frames_remaining;

        float* chunk_out = safe_output ? safe_output + offset * output_channels : nullptr;
        const float* chunk_in = safe_input ? safe_input + offset * input_channels : nullptr;

        callbacks->read([&](const auto& list) {
            for (auto* cb : list) {
                cb->process(chunk_out, chunk_in, chunk, input_channels, output_channels);
            }
        });

        offset += chunk;
        frames_remaining -= chunk;
    }
}

void AudioDeviceManager::data_callback(void* p_device_void,
                                      void* p_output,
                                      const void* p_input,
                                      uint32_t frame_count) {
    auto* p_device = static_cast<ma_device*>(p_device_void);
    auto* user_data = static_cast<DeviceUserData*>(p_device->pUserData);
    if (!user_data || !user_data->m_manager) { return; }

    user_data->m_manager->process_callbacks(user_data->m_role,
                                        p_device,
                                        static_cast<float*>(p_output),
                                        static_cast<const float*>(p_input),
                                        frame_count);
}

void AudioDeviceManager::notification_callback(const void* p_notification_void) {
    auto* p_notification = static_cast<const ma_device_notification*>(p_notification_void);
    auto* user_data = static_cast<DeviceUserData*>(p_notification->pDevice->pUserData);
    if (!user_data || !user_data->m_manager) { return; }

    auto cb = std::atomic_load_explicit(&user_data->m_manager->m_notification_callback, std::memory_order_acquire);
    if (cb) { (*cb)(to_notification_type(p_notification->type)); }
}

void AudioDeviceManager::static_log_callback(void* p_user_data, uint32_t level, const char* p_message) {
    auto* impl = static_cast<Impl*>(p_user_data);
    if (!impl || !impl->m_log_callback) { return; }

    // Miniaudio levels: 1=error, 2=warning, 3=info, 4=debug.
    Logger::LogLevel normalised;
    switch (level) {
        case 1: normalised = Logger::LogLevel::Error; break;
        case 2: normalised = Logger::LogLevel::Warning; break;
        case 3: normalised = Logger::LogLevel::Info; break;
        case 4: normalised = Logger::LogLevel::Debug; break;
        default: normalised = Logger::LogLevel::Info; break;
    }
    impl->m_log_callback(normalised, p_message);
}

}  // namespace thl
