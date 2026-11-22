#include <jni.h>
#include <tanh/core/AbiNamespace.h>
#include <tanh/core/Clock.h>
#include <tanh/core/Logger.h>
#include <tanh/midi/AndroidMidi.h>
#include <tanh/midi/DeviceInfo.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "DeviceBackend.h"
#include "PortReader.h"

// Android backend: android.media.midi through the Java class MidiBridge
// (android/java/tanhlab/tanhlib/midi/MidiBridge.java), which the app compiles in.
//
// Java only forwards; the state lives here. Each DeviceManager owns one State and one
// MidiBridge. Java reaches its State through a handle that is looked up in a registry, not
// through a pointer, so a callback that arrives after the DeviceManager is gone finds
// nothing instead of freed memory.
//
// Lock order: MidiBridge's monitor, then State::m_mutex. Calls into Java made while
// m_mutex is held (open, close) only post to MidiBridge's handler thread; start() and
// stop(), which take the monitor, are called without it.

namespace thl::inline THL_ABI_NAMESPACE::midi {

namespace {

constexpr const char* k_log_group = "thl.midi.device_manager";
constexpr const char* k_bridge_class = "tanhlab/tanhlib/midi/MidiBridge";

// android.media.midi.MidiDeviceInfo.TYPE_*
constexpr jint k_type_usb = 1;
constexpr jint k_type_virtual = 2;
constexpr jint k_type_bluetooth = 3;

// What set_android_java_vm() resolved. Written once, during startup, before any
// DeviceManager exists; m_ready publishes it.
struct Jni {
    JavaVM* m_vm = nullptr;
    jclass m_bridge_class = nullptr;  // global reference
    jmethodID m_create = nullptr;
    jmethodID m_start = nullptr;
    jmethodID m_open = nullptr;
    jmethodID m_close = nullptr;
    jmethodID m_stop = nullptr;
    std::atomic<bool> m_ready{false};
};

Jni& jni() {
    static Jni instance;
    return instance;
}

// The calling thread's JNIEnv, attaching the thread for the scope if it is a native one.
class ScopedEnv {
public:
    ScopedEnv() {
        JavaVM* vm = jni().m_vm;
        if (vm == nullptr) { return; }
        const jint status = vm->GetEnv(reinterpret_cast<void**>(&m_env), JNI_VERSION_1_6);
        if (status == JNI_EDETACHED) {
            if (vm->AttachCurrentThread(&m_env, nullptr) == JNI_OK) {
                m_attached = true;
            } else {
                m_env = nullptr;
            }
        } else if (status != JNI_OK) {
            m_env = nullptr;
        }
    }
    ~ScopedEnv() {
        if (m_attached) { jni().m_vm->DetachCurrentThread(); }
    }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;
    ScopedEnv(ScopedEnv&&) = delete;
    ScopedEnv& operator=(ScopedEnv&&) = delete;

    [[nodiscard]] JNIEnv* get() const { return m_env; }

private:
    JNIEnv* m_env = nullptr;
    bool m_attached = false;
};

// True, after clearing and logging it, when the last JNI call threw.
bool failed(JNIEnv* env, const char* what) {
    if (env->ExceptionCheck() == JNI_FALSE) { return false; }
    env->ExceptionClear();
    Logger::logf(Logger::LogLevel::Warning, k_log_group, "Java exception in %s", what);
    return true;
}

std::string to_string(JNIEnv* env, jstring string) {
    if (string == nullptr) { return {}; }
    const char* chars = env->GetStringUTFChars(string, nullptr);
    if (chars == nullptr) { return {}; }
    std::string result(chars);
    env->ReleaseStringUTFChars(string, chars);
    return result;
}

// The application Context, without the app handing one in: the same route AudioIO takes
// (src/audio-io/AndroidAudioDevices.cpp). A local reference, or null.
jobject application_context(JNIEnv* env) {
    jclass activity_thread = env->FindClass("android/app/ActivityThread");
    if (failed(env, "FindClass(ActivityThread)") || activity_thread == nullptr) { return nullptr; }
    jmethodID current_application = env->GetStaticMethodID(activity_thread,
                                                           "currentApplication",
                                                           "()Landroid/app/Application;");
    if (failed(env, "currentApplication lookup") || current_application == nullptr) {
        env->DeleteLocalRef(activity_thread);
        return nullptr;
    }
    jobject context = env->CallStaticObjectMethod(activity_thread, current_application);
    env->DeleteLocalRef(activity_thread);
    if (failed(env, "currentApplication")) { return nullptr; }
    return context;
}

Transport transport_of(jint type) {
    switch (type) {
        case k_type_usb: return Transport::Usb;
        case k_type_bluetooth: return Transport::Bluetooth;
        case k_type_virtual: return Transport::Virtual;
        default: return Transport::Unknown;
    }
}

class State {
public:
    explicit State(BackendContext context)
        : m_context(std::move(context)), m_auto_open(m_context.m_auto_open) {}

    // Creates the Java half and has it report the devices that are present. `handle` is
    // this State's key in the registry.
    void start(JNIEnv* env, jlong handle) {
        jobject context = application_context(env);
        if (context == nullptr) {
            Logger::logf(Logger::LogLevel::Warning,
                         k_log_group,
                         "no application context; no MIDI input");
            return;
        }
        jobject bridge = env->CallStaticObjectMethod(
            jni().m_bridge_class,
            jni().m_create,
            context,
            handle,
            static_cast<jboolean>(m_context.m_protocol == Protocol::Midi2 ? JNI_TRUE : JNI_FALSE));
        env->DeleteLocalRef(context);
        if (failed(env, "MidiBridge.create") || bridge == nullptr) {
            Logger::logf(Logger::LogLevel::Info, k_log_group, "this device has no MIDI support");
            return;
        }
        {
            const std::scoped_lock lock(m_mutex);
            m_bridge = env->NewGlobalRef(bridge);
            m_starting = true;
        }
        env->DeleteLocalRef(bridge);

        // Reports every present device through on_device_added(), on this thread.
        env->CallVoidMethod(m_bridge, jni().m_start);
        failed(env, "MidiBridge.start");

        const std::scoped_lock lock(m_mutex);
        m_starting = false;
    }

    void shut_down(JNIEnv* env) {
        jobject bridge = nullptr;
        {
            const std::scoped_lock lock(m_mutex);
            bridge = std::exchange(m_bridge, nullptr);
            m_devices.clear();
        }
        if (bridge == nullptr || env == nullptr) { return; }
        env->CallVoidMethod(bridge, jni().m_stop);
        failed(env, "MidiBridge.stop");
        env->DeleteGlobalRef(bridge);
    }

    [[nodiscard]] bool running() const {
        const std::scoped_lock lock(m_mutex);
        return m_bridge != nullptr;
    }

    [[nodiscard]] std::vector<DeviceInfo> input_devices() const {
        const std::scoped_lock lock(m_mutex);
        std::vector<DeviceInfo> devices;
        devices.reserve(m_devices.size());
        for (const Device& device : m_devices) { devices.push_back(device.m_info); }
        return devices;
    }

    bool open_input(JNIEnv* env, std::uint32_t id) {
        const std::scoped_lock lock(m_mutex);
        const auto it = find_locked(id);
        if (it == m_devices.end()) { return false; }
        return request_open_locked(env, *it);
    }

    bool close_input(JNIEnv* env, std::uint32_t id) {
        const std::scoped_lock lock(m_mutex);
        const auto it = find_locked(id);
        if (it == m_devices.end()) { return false; }
        it->m_info.m_open = false;
        it->m_opening = false;
        if (m_bridge != nullptr && env != nullptr) {
            env->CallVoidMethod(m_bridge, jni().m_close, static_cast<jint>(id));
            failed(env, "MidiBridge.close");
        }
        return true;
    }

    void set_auto_open(JNIEnv* env, bool auto_open) {
        const std::scoped_lock lock(m_mutex);
        m_auto_open = auto_open;
        if (!auto_open) { return; }
        for (Device& device : m_devices) { request_open_locked(env, device); }
    }

    // ── Called by MidiBridge ──────────────────────────────────────────────────

    void on_device_added(JNIEnv* env, DeviceInfo info, bool ump, std::size_t port_count) {
        bool notify = false;
        {
            const std::scoped_lock lock(m_mutex);
            if (m_bridge == nullptr || find_locked(info.m_id) != m_devices.end()) { return; }

            Device device;
            device.m_info = std::move(info);
            device.m_info.m_source = m_next_source++;
            device.m_ports.assign(port_count, PortReader(ump));
            m_devices.push_back(std::move(device));
            if (m_auto_open) { request_open_locked(env, m_devices.back()); }
            notify = !m_starting;
        }
        if (notify && m_context.m_on_change) { m_context.m_on_change(); }
    }

    void on_device_removed(std::uint32_t id) {
        {
            const std::scoped_lock lock(m_mutex);
            const auto it = find_locked(id);
            if (it == m_devices.end()) { return; }
            m_devices.erase(it);
        }
        if (m_context.m_on_change) { m_context.m_on_change(); }
    }

    void on_opened(std::uint32_t id, bool ok) {
        {
            const std::scoped_lock lock(m_mutex);
            const auto it = find_locked(id);
            // Closed again while it was opening: the close request is already on its way.
            if (it == m_devices.end() || !it->m_opening) { return; }
            it->m_opening = false;
            it->m_info.m_open = ok;
            if (ok) {
                for (PortReader& port : it->m_ports) { port.reset(); }
            } else {
                Logger::logf(Logger::LogLevel::Warning,
                             k_log_group,
                             "could not open MIDI input '%s'",
                             it->m_info.m_name.c_str());
            }
        }
        if (m_context.m_on_change) { m_context.m_on_change(); }
    }

    // Runs on the thread android.media.midi keeps per port — an ordinary Java thread, so
    // the mutex and the copy out of the Java array are fine here; the audio thread only
    // ever touches the queue.
    void on_data(JNIEnv* env,
                 std::uint16_t source,
                 std::size_t port,
                 jbyteArray data,
                 jint offset,
                 jint count,
                 std::int64_t timestamp) {
        const std::scoped_lock lock(m_mutex);
        const auto it = std::ranges::find_if(m_devices, [source](const Device& device) {
            return device.m_info.m_source == source;
        });
        if (it == m_devices.end() || !it->m_info.m_open || port >= it->m_ports.size()) { return; }

        // System.nanoTime(), the clock of core::uptime_ns() on Android; 0 means "now".
        const std::int64_t time_ns = timestamp != 0 ? timestamp : core::uptime_ns();

        std::array<jbyte, k_chunk_size> chunk{};
        jint done = 0;
        while (done < count) {
            const jint size = std::min<jint>(count - done, static_cast<jint>(chunk.size()));
            env->GetByteArrayRegion(data, offset + done, size, chunk.data());
            if (failed(env, "GetByteArrayRegion")) { return; }
            it->m_ports[port].read(reinterpret_cast<const std::uint8_t*>(chunk.data()),
                                   static_cast<std::size_t>(size),
                                   time_ns,
                                   source,
                                   m_context.m_receive_sysex,
                                   *m_context.m_queue);
            done += size;
        }
    }

private:
    // android.media.midi hands over at most 1015 data bytes per call.
    static constexpr std::size_t k_chunk_size = 1024;

    struct Device {
        DeviceInfo m_info;
        std::vector<PortReader> m_ports;  // one stream state per output port
        bool m_opening = false;           // an open request is under way
    };

    [[nodiscard]] std::vector<Device>::iterator find_locked(std::uint32_t id) {
        return std::ranges::find_if(m_devices, [id](const Device& device) {
            return device.m_info.m_id == id;
        });
    }

    // Asks Java to open the device. Opening is asynchronous: on_opened() reports the
    // result. Returns whether the device is open or being opened.
    bool request_open_locked(JNIEnv* env, Device& device) {
        if (device.m_info.m_open || device.m_opening) { return true; }
        if (m_bridge == nullptr || env == nullptr) { return false; }
        env->CallVoidMethod(m_bridge,
                            jni().m_open,
                            static_cast<jint>(device.m_info.m_id),
                            static_cast<jint>(device.m_info.m_source));
        if (failed(env, "MidiBridge.open")) { return false; }
        device.m_opening = true;
        return true;
    }

    BackendContext m_context;
    mutable std::mutex m_mutex;
    std::vector<Device> m_devices;
    jobject m_bridge = nullptr;  // global reference
    std::uint16_t m_next_source = 0;
    bool m_auto_open = true;
    bool m_starting = false;  // inside start(): the initial devices are not "changes"
};

// handle -> State, for the callbacks from Java.
struct Registry {
    std::mutex m_mutex;
    std::unordered_map<jlong, std::shared_ptr<State>> m_states;
    jlong m_next_handle = 1;
};

Registry& registry() {
    static Registry instance;
    return instance;
}

std::shared_ptr<State> find_state(jlong handle) {
    Registry& reg = registry();
    const std::scoped_lock lock(reg.m_mutex);
    const auto it = reg.m_states.find(handle);
    return it == reg.m_states.end() ? nullptr : it->second;
}

// ── Native methods of MidiBridge ──────────────────────────────────────────────

void JNICALL native_on_device_added(JNIEnv* env,
                                    jclass /*clazz*/,
                                    jlong handle,
                                    jint id,
                                    jstring name,
                                    jstring manufacturer,
                                    jstring key,
                                    jint type,
                                    jboolean ump,
                                    jint output_ports) {
    const auto state = find_state(handle);
    if (!state || output_ports <= 0) { return; }

    DeviceInfo info;
    info.m_id = static_cast<std::uint32_t>(id);
    info.m_name = to_string(env, name);
    info.m_manufacturer = to_string(env, manufacturer);
    info.m_persistent_key = to_string(env, key);
    info.m_transport = transport_of(type);
    info.m_protocol = ump == JNI_TRUE ? Protocol::Midi2 : Protocol::Midi1;
    state->on_device_added(env,
                           std::move(info),
                           ump == JNI_TRUE,
                           static_cast<std::size_t>(output_ports));
}

void JNICALL native_on_device_removed(JNIEnv* /*env*/, jclass /*clazz*/, jlong handle, jint id) {
    if (const auto state = find_state(handle)) {
        state->on_device_removed(static_cast<std::uint32_t>(id));
    }
}

void JNICALL
    native_on_opened(JNIEnv* /*env*/, jclass /*clazz*/, jlong handle, jint id, jboolean ok) {
    if (const auto state = find_state(handle)) {
        state->on_opened(static_cast<std::uint32_t>(id), ok == JNI_TRUE);
    }
}

void JNICALL native_on_data(JNIEnv* env,
                            jclass /*clazz*/,
                            jlong handle,
                            jint slot,
                            jint port,
                            jboolean /*ump*/,
                            jbyteArray data,
                            jint offset,
                            jint count,
                            jlong timestamp) {
    if (data == nullptr || count <= 0 || offset < 0 || port < 0) { return; }
    if (const auto state = find_state(handle)) {
        state->on_data(env,
                       static_cast<std::uint16_t>(slot),
                       static_cast<std::size_t>(port),
                       data,
                       offset,
                       count,
                       timestamp);
    }
}

class AndroidBackend final : public DeviceBackend {
public:
    explicit AndroidBackend(BackendContext context)
        : m_state(std::make_shared<State>(std::move(context))) {
        if (!jni().m_ready.load(std::memory_order_acquire)) {
            Logger::logf(Logger::LogLevel::Warning,
                         k_log_group,
                         "thl::midi::set_android_java_vm() was not called, or MidiBridge is "
                         "not part of the app; no MIDI input");
            return;
        }
        {
            Registry& reg = registry();
            const std::scoped_lock lock(reg.m_mutex);
            m_handle = reg.m_next_handle++;
            reg.m_states.emplace(m_handle, m_state);
        }
        const ScopedEnv env;
        if (env.get() != nullptr) { m_state->start(env.get(), m_handle); }
    }

    ~AndroidBackend() override {
        {
            const ScopedEnv env;
            m_state->shut_down(env.get());
        }
        // After stop() Java reports nothing more; a call that was already under way holds
        // its own reference to the State.
        Registry& reg = registry();
        const std::scoped_lock lock(reg.m_mutex);
        reg.m_states.erase(m_handle);
    }

    AndroidBackend(const AndroidBackend&) = delete;
    AndroidBackend& operator=(const AndroidBackend&) = delete;
    AndroidBackend(AndroidBackend&&) = delete;
    AndroidBackend& operator=(AndroidBackend&&) = delete;

    [[nodiscard]] bool running() const override { return m_state->running(); }
    [[nodiscard]] std::vector<DeviceInfo> input_devices() const override {
        return m_state->input_devices();
    }
    bool open_input(std::uint32_t id) override {
        const ScopedEnv env;
        return m_state->open_input(env.get(), id);
    }
    bool close_input(std::uint32_t id) override {
        const ScopedEnv env;
        return m_state->close_input(env.get(), id);
    }
    void set_auto_open(bool auto_open) override {
        const ScopedEnv env;
        m_state->set_auto_open(env.get(), auto_open);
    }
    // android.media.midi offers virtual devices only as a declared Service of the app.
    bool create_virtual_input(const std::string& /*name*/) override { return false; }

private:
    std::shared_ptr<State> m_state;
    jlong m_handle = 0;
};

}  // namespace

void set_android_java_vm(void* java_vm) {
    Jni& j = jni();
    if (j.m_ready.load(std::memory_order_acquire) || java_vm == nullptr) { return; }
    j.m_vm = static_cast<JavaVM*>(java_vm);

    const ScopedEnv scoped;
    JNIEnv* env = scoped.get();
    if (env == nullptr) { return; }

    // Only here, on the thread that loads the library, is the app's class loader current.
    jclass local = env->FindClass(k_bridge_class);
    if (failed(env, "FindClass(MidiBridge)") || local == nullptr) {
        Logger::logf(Logger::LogLevel::Warning,
                     k_log_group,
                     "class %s is not part of the app; no MIDI input",
                     k_bridge_class);
        return;
    }
    j.m_bridge_class = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);

    // Signatures as javap -s prints them for MidiBridge.
    j.m_create =
        env->GetStaticMethodID(j.m_bridge_class,
                               "create",
                               "(Landroid/content/Context;JZ)Ltanhlab/tanhlib/midi/MidiBridge;");
    j.m_start = env->GetMethodID(j.m_bridge_class, "start", "()V");
    j.m_open = env->GetMethodID(j.m_bridge_class, "open", "(II)V");
    j.m_close = env->GetMethodID(j.m_bridge_class, "close", "(I)V");
    j.m_stop = env->GetMethodID(j.m_bridge_class, "stop", "()V");
    if (failed(env, "MidiBridge method lookup")) { return; }

    const std::array<JNINativeMethod, 4> natives = {{
        {.name = "nativeOnDeviceAdded",
         .signature = "(JILjava/lang/String;Ljava/lang/String;Ljava/lang/String;IZI)V",
         .fnPtr = reinterpret_cast<void*>(&native_on_device_added)},
        {.name = "nativeOnDeviceRemoved",
         .signature = "(JI)V",
         .fnPtr = reinterpret_cast<void*>(&native_on_device_removed)},
        {.name = "nativeOnOpened",
         .signature = "(JIZ)V",
         .fnPtr = reinterpret_cast<void*>(&native_on_opened)},
        {.name = "nativeOnData",
         .signature = "(JIIZ[BIIJ)V",
         .fnPtr = reinterpret_cast<void*>(&native_on_data)},
    }};
    if (env->RegisterNatives(j.m_bridge_class, natives.data(), static_cast<jint>(natives.size())) !=
            JNI_OK ||
        failed(env, "RegisterNatives")) {
        Logger::logf(Logger::LogLevel::Warning,
                     k_log_group,
                     "could not bind the native methods of %s; no MIDI input",
                     k_bridge_class);
        return;
    }
    j.m_ready.store(true, std::memory_order_release);
}

bool device_backend_supported() {
    return true;
}

std::unique_ptr<DeviceBackend> make_device_backend(BackendContext context) {
    return std::make_unique<AndroidBackend>(std::move(context));
}

}  // namespace thl::inline THL_ABI_NAMESPACE::midi
