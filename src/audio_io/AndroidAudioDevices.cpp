#if defined(THL_PLATFORM_ANDROID)

#include <tanh/audio_io/AndroidAudioDevices.h>
#include "miniaudio.h"
#include <tanh/core/Logger.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <jni.h>
#include <thread>

namespace thl {

static JavaVM* g_javaVM = nullptr;
static std::atomic<bool> g_scoEnabled{false};

void setAndroidJavaVM(void* javaVM) {
    g_javaVM = static_cast<JavaVM*>(javaVM);
}

bool isAndroidBluetoothScoEnabled() {
    return g_scoEnabled.load(std::memory_order_relaxed);
}

// Maps Android AudioDeviceInfo type constants to human-readable names.
// See android.media.AudioDeviceInfo.TYPE_* constants.
static const char* androidDeviceTypeName(int type) {
    switch (type) {
        case 1:  return "Earpiece";
        case 2:  return "Speaker";
        case 3:  return "Wired Headset";
        case 4:  return "Wired Headphones";
        case 7:  return "Bluetooth SCO";
        case 8:  return "Bluetooth A2DP";
        case 9:  return "HDMI";
        case 11: return "USB Device";
        case 13: return "Telephony";
        case 15: return "Line In";
        case 17: return "Line Out";
        case 19: return "Microphone Array";
        case 22: return "USB Headset";
        case 24: return "Hearing Aid";
        case 25: return "Built-in Mic";
        case 26: return "BLE Headset";
        case 27: return "BLE Speaker";
        default: return "Audio Device";
    }
}

// Returns true for device types suitable for media playback/capture.
// SCO (type 7) is only included when the SCO link has been started.
// Earpiece (1) and Telephony (13) are voice-call paths — always excluded.
static bool isSupportedOutputType(int type) {
    switch (type) {
        case 2:  // Speaker
        case 3:  // Wired Headset
        case 4:  // Wired Headphones
            return true;
        case 8:  // Bluetooth A2DP — hide when SCO/HFP is active
            return !g_scoEnabled.load(std::memory_order_relaxed);
        case 9:  // HDMI
        case 11: // USB Device
        case 17: // Line Out
        case 22: // USB Headset
        case 24: // Hearing Aid
        case 26: // BLE Headset
        case 27: // BLE Speaker
            return true;
        case 7:  // Bluetooth SCO — only when SCO link is active
            return g_scoEnabled.load(std::memory_order_relaxed);
        default:
            return false;
    }
}

static bool isSupportedInputType(int type) {
    switch (type) {
        case 3:  // Wired Headset (has mic)
        case 11: // USB Device
        case 15: // Line In
        case 19: // Microphone Array
        case 22: // USB Headset
        case 25: // Built-in Mic (TYPE_BUILTIN_MIC)
            return true;
        case 7:  // Bluetooth SCO — only when SCO link is active
            return g_scoEnabled.load(std::memory_order_relaxed);
        default:
            return false;
    }
}

// RAII helper that detaches thecurrent thread from the JVM if we attached it.
struct ScopedJNIEnv {
    JNIEnv* env = nullptr;
    bool needsDetach = false;

    explicit ScopedJNIEnv(JavaVM* vm) {
        jint res = vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
        if (res == JNI_EDETACHED) {
            if (vm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
                needsDetach = true;
            } else {
                env = nullptr;
            }
        } else if (res != JNI_OK) {
            env = nullptr;
        }
    }

    ~ScopedJNIEnv() {
        if (needsDetach && g_javaVM) {
            g_javaVM->DetachCurrentThread();
        }
    }

    explicit operator bool() const { return env != nullptr; }
};

// Helper: obtain the AudioManager jobject.  Caller must DeleteLocalRef the
// returned object and all intermediate refs.
static jobject getAudioManager(JNIEnv* env) {
    jclass activityThreadClass = env->FindClass("android/app/ActivityThread");
    if (!activityThreadClass || env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }

    jmethodID currentAppMethod = env->GetStaticMethodID(
        activityThreadClass, "currentApplication", "()Landroid/app/Application;");
    jobject appContext = env->CallStaticObjectMethod(activityThreadClass, currentAppMethod);
    if (!appContext || env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }

    jclass contextClass = env->FindClass("android/content/Context");
    jfieldID audioServiceField = env->GetStaticFieldID(
        contextClass, "AUDIO_SERVICE", "Ljava/lang/String;");
    jobject audioServiceStr = env->GetStaticObjectField(contextClass, audioServiceField);

    jmethodID getSystemService = env->GetMethodID(
        contextClass, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    jobject audioManager = env->CallObjectMethod(appContext, getSystemService, audioServiceStr);

    env->DeleteLocalRef(audioServiceStr);
    env->DeleteLocalRef(appContext);

    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    return audioManager;
}

bool setAndroidBluetoothSco(bool enable) {
    if (!g_javaVM) return false;

    ScopedJNIEnv jni(g_javaVM);
    if (!jni) return false;
    JNIEnv* env = jni.env;

    jobject audioManager = getAudioManager(env);
    if (!audioManager) return false;

    jclass audioManagerClass = env->FindClass("android/media/AudioManager");
    jmethodID setMode = env->GetMethodID(audioManagerClass, "setMode", "(I)V");

    if (enable) {
        // MODE_IN_COMMUNICATION (3) is required for SCO input to be routed.
        env->CallVoidMethod(audioManager, setMode, static_cast<jint>(3));

        jmethodID startSco = env->GetMethodID(audioManagerClass, "startBluetoothSco", "()V");
        env->CallVoidMethod(audioManager, startSco);

        jmethodID setScoOn = env->GetMethodID(audioManagerClass, "setBluetoothScoOn", "(Z)V");
        env->CallVoidMethod(audioManager, setScoOn, static_cast<jboolean>(JNI_TRUE));

        // startBluetoothSco() is asynchronous — poll isBluetoothScoOn() until
        // the SCO audio link is established or we time out (~2 s).
        jmethodID isScoOn = env->GetMethodID(audioManagerClass, "isBluetoothScoOn", "()Z");
        constexpr int kMaxAttempts = 40;       // 40 × 50 ms = 2 s
        constexpr int kPollIntervalMs = 50;
        bool connected = false;
        for (int i = 0; i < kMaxAttempts; ++i) {
            if (env->CallBooleanMethod(audioManager, isScoOn)) {
                connected = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
        }
        if (!connected) {
            thl::Logger::logf(thl::Logger::LogLevel::Warning,
                              "thl.audio_io.android_devices",
                              "Bluetooth SCO did not connect within timeout");
        }
    } else {
        jmethodID setScoOn = env->GetMethodID(audioManagerClass, "setBluetoothScoOn", "(Z)V");
        env->CallVoidMethod(audioManager, setScoOn, static_cast<jboolean>(JNI_FALSE));

        jmethodID stopSco = env->GetMethodID(audioManagerClass, "stopBluetoothSco", "()V");
        env->CallVoidMethod(audioManager, stopSco);

        // MODE_NORMAL (0) — restore default audio routing.
        env->CallVoidMethod(audioManager, setMode, static_cast<jint>(0));
    }

    env->DeleteLocalRef(audioManager);

    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        thl::Logger::logf(thl::Logger::LogLevel::Error,
                          "thl.audio_io.android_devices",
                          "Failed to %s Bluetooth SCO",
                          enable ? "start" : "stop");
        return false;
    }

    g_scoEnabled.store(enable, std::memory_order_relaxed);
    thl::Logger::logf(thl::Logger::LogLevel::Info,
                      "thl.audio_io.android_devices",
                      "Bluetooth SCO %s", enable ? "started" : "stopped");
    return true;
}

std::vector<AudioDeviceInfo> enumerateAndroidAudioDevices(DeviceType type) {
    std::vector<AudioDeviceInfo> result;

    if (!g_javaVM) {
        thl::Logger::logf(thl::Logger::LogLevel::Warning,
                          "thl.audio_io.android_devices",
                          "JavaVM not set — cannot enumerate Android audio devices");
        return result;
    }

    ScopedJNIEnv jni(g_javaVM);
    if (!jni) return result;
    JNIEnv* env = jni.env;

    jobject audioManager = getAudioManager(env);
    if (!audioManager) return result;

    // --- Call AudioManager.getDevices(flags) ---
    // GET_DEVICES_OUTPUTS = 2, GET_DEVICES_INPUTS = 1
    jclass audioManagerClass = env->FindClass("android/media/AudioManager");
    jmethodID getDevicesMethod = env->GetMethodID(
        audioManagerClass, "getDevices", "(I)[Landroid/media/AudioDeviceInfo;");

    int flags = (type == DeviceType::Playback) ? 2 : 1;
    auto devices = static_cast<jobjectArray>(
        env->CallObjectMethod(audioManager, getDevicesMethod, flags));
    if (!devices || env->ExceptionCheck()) {
        env->ExceptionClear();
        return result;
    }

    jsize count = env->GetArrayLength(devices);

    // --- Cache AudioDeviceInfo method IDs ---
    jclass deviceInfoClass = env->FindClass("android/media/AudioDeviceInfo");
    jmethodID getIdMethod        = env->GetMethodID(deviceInfoClass, "getId", "()I");
    jmethodID getProductName     = env->GetMethodID(deviceInfoClass, "getProductName",
                                                     "()Ljava/lang/CharSequence;");
    jmethodID getTypeMethod      = env->GetMethodID(deviceInfoClass, "getType", "()I");
    jmethodID getSampleRates     = env->GetMethodID(deviceInfoClass, "getSampleRates", "()[I");

    jclass charSeqClass = env->FindClass("java/lang/CharSequence");
    jmethodID toStringMethod = env->GetMethodID(charSeqClass, "toString", "()Ljava/lang/String;");

    for (jsize i = 0; i < count; i++) {
        jobject deviceObj = env->GetObjectArrayElement(devices, i);

        int id         = env->CallIntMethod(deviceObj, getIdMethod);
        int deviceType = env->CallIntMethod(deviceObj, getTypeMethod);

        // Skip device types that can't be opened with AAudio usage_media.
        bool supported = (type == DeviceType::Playback)
                             ? isSupportedOutputType(deviceType)
                             : isSupportedInputType(deviceType);
        if (!supported) {
            env->DeleteLocalRef(deviceObj);
            continue;
        }

        // --- Build display name ---
        std::string productName;
        jobject productNameCS = env->CallObjectMethod(deviceObj, getProductName);
        if (productNameCS) {
            auto nameStr = static_cast<jstring>(
                env->CallObjectMethod(productNameCS, toStringMethod));
            if (nameStr) {
                const char* chars = env->GetStringUTFChars(nameStr, nullptr);
                if (chars) {
                    productName = chars;
                    env->ReleaseStringUTFChars(nameStr, chars);
                }
                env->DeleteLocalRef(nameStr);
            }
            env->DeleteLocalRef(productNameCS);
        }

        std::string displayName = androidDeviceTypeName(deviceType);
        if (!productName.empty() && productName != displayName) {
            displayName += " (" + productName + ")";
        }

        // --- Get supported sample rates ---
        std::vector<uint32_t> sampleRates;
        auto ratesArray = static_cast<jintArray>(
            env->CallObjectMethod(deviceObj, getSampleRates));
        if (ratesArray) {
            jsize rateCount = env->GetArrayLength(ratesArray);
            if (rateCount > 0) {
                jint* rates = env->GetIntArrayElements(ratesArray, nullptr);
                for (jsize r = 0; r < rateCount; r++) {
                    sampleRates.push_back(static_cast<uint32_t>(rates[r]));
                }
                env->ReleaseIntArrayElements(ratesArray, rates, 0);
            }
            env->DeleteLocalRef(ratesArray);
        }
        // Empty array → device supports all common rates.
        if (sampleRates.empty()) {
            sampleRates = {22050, 44100, 48000, 96000};
        }
        std::sort(sampleRates.begin(), sampleRates.end());

        // --- Populate AudioDeviceInfo ---
        AudioDeviceInfo info;
        info.name        = displayName;
        info.deviceType  = type;
        info.sampleRates = std::move(sampleRates);

        // Store the Android device ID so miniaudio's AAudio backend can open
        // the specific device via AAudioStreamBuilder_setDeviceId().
        ma_device_id maId{};
        maId.aaudio = static_cast<ma_int32>(id);
        static_assert(sizeof(ma_device_id) <= AudioDeviceInfo::kDeviceIdStorageSize,
                      "ma_device_id too large for storage");
        std::memcpy(info.deviceIdStoragePtr(), &maId, sizeof(ma_device_id));

        result.push_back(std::move(info));

        env->DeleteLocalRef(deviceObj);
    }

    // --- Cleanup local references ---
    env->DeleteLocalRef(devices);
    env->DeleteLocalRef(audioManager);

    // Remove duplicate entries (Android may report the same logical device
    // more than once, e.g. built-in mic with different channel masks).
    {
        std::vector<AudioDeviceInfo> unique;
        unique.reserve(result.size());
        for (auto& dev : result) {
            if (std::none_of(unique.begin(), unique.end(),
                             [&](const AudioDeviceInfo& u) { return u.name == dev.name; })) {
                unique.push_back(std::move(dev));
            }
        }
        result = std::move(unique);
    }

    thl::Logger::logf(thl::Logger::LogLevel::Debug,
                      "thl.audio_io.android_devices",
                      "Enumerated %zu %s devices via Android AudioManager",
                      result.size(),
                      type == DeviceType::Playback ? "output" : "input");
    return result;
}

// ---------------------------------------------------------------------------
// Active device name helpers — query the current audio route via
// MediaRouter or AudioManager to match what AAudio is actually using.
// ---------------------------------------------------------------------------

static std::string getAndroidActiveDeviceName(int flags, int32_t targetDeviceId) {
    if (!g_javaVM) return {};

    ScopedJNIEnv jni(g_javaVM);
    if (!jni) return {};
    JNIEnv* env = jni.env;

    jobject audioManager = getAudioManager(env);
    if (!audioManager) return {};

    jclass audioManagerClass = env->FindClass("android/media/AudioManager");
    jmethodID getDevicesMethod = env->GetMethodID(
        audioManagerClass, "getDevices", "(I)[Landroid/media/AudioDeviceInfo;");

    auto devices = static_cast<jobjectArray>(
        env->CallObjectMethod(audioManager, getDevicesMethod, flags));
    if (!devices || env->ExceptionCheck()) {
        env->ExceptionClear();
        env->DeleteLocalRef(audioManager);
        return {};
    }

    jsize count = env->GetArrayLength(devices);

    jclass deviceInfoClass = env->FindClass("android/media/AudioDeviceInfo");
    jmethodID getIdMethod    = env->GetMethodID(deviceInfoClass, "getId", "()I");
    jmethodID getTypeMethod  = env->GetMethodID(deviceInfoClass, "getType", "()I");
    jmethodID getProductName = env->GetMethodID(deviceInfoClass, "getProductName",
                                                 "()Ljava/lang/CharSequence;");
    jclass charSeqClass = env->FindClass("java/lang/CharSequence");
    jmethodID toStringMethod = env->GetMethodID(charSeqClass, "toString",
                                                 "()Ljava/lang/String;");

    bool isOutput = (flags == 2);
    std::string result;
    std::string firstSupported;  // fallback if ID not found

    for (jsize i = 0; i < count; i++) {
        jobject deviceObj = env->GetObjectArrayElement(devices, i);
        int deviceType = env->CallIntMethod(deviceObj, getTypeMethod);

        bool supported = isOutput ? isSupportedOutputType(deviceType)
                                  : isSupportedInputType(deviceType);
        if (!supported) {
            env->DeleteLocalRef(deviceObj);
            continue;
        }

        // Build display name using same format as enumerateAndroidAudioDevices
        std::string displayName = androidDeviceTypeName(deviceType);
        jobject productNameCS = env->CallObjectMethod(deviceObj, getProductName);
        if (productNameCS) {
            auto nameStr = static_cast<jstring>(
                env->CallObjectMethod(productNameCS, toStringMethod));
            if (nameStr) {
                const char* chars = env->GetStringUTFChars(nameStr, nullptr);
                if (chars && chars[0] != '\0') {
                    std::string productName = chars;
                    if (productName != displayName) {
                        displayName += " (" + productName + ")";
                    }
                    env->ReleaseStringUTFChars(nameStr, chars);
                }
                env->DeleteLocalRef(nameStr);
            }
            env->DeleteLocalRef(productNameCS);
        }

        // Remember the first supported device as fallback
        if (firstSupported.empty()) {
            firstSupported = displayName;
        }

        // If a specific device ID was requested, match by ID
        if (targetDeviceId > 0) {
            int id = env->CallIntMethod(deviceObj, getIdMethod);
            if (id == targetDeviceId) {
                result = std::move(displayName);
                env->DeleteLocalRef(deviceObj);
                break;
            }
        } else {
            // No specific ID — return first supported device
            result = std::move(displayName);
            env->DeleteLocalRef(deviceObj);
            break;
        }

        env->DeleteLocalRef(deviceObj);
    }

    // If we had a specific ID but didn't find it (device disconnected),
    // fall back to the first supported device.
    if (result.empty() && !firstSupported.empty()) {
        result = std::move(firstSupported);
    }

    env->DeleteLocalRef(devices);
    env->DeleteLocalRef(audioManager);
    return result;
}

std::string getAndroidActiveOutputDeviceName(int32_t aaudioDeviceId) {
    return getAndroidActiveDeviceName(/* GET_DEVICES_OUTPUTS */ 2, aaudioDeviceId);
}

std::string getAndroidActiveInputDeviceName(int32_t aaudioDeviceId) {
    return getAndroidActiveDeviceName(/* GET_DEVICES_INPUTS */ 1, aaudioDeviceId);
}

}  // namespace thl

#endif  // THL_PLATFORM_ANDROID
