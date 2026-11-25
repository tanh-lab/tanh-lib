#include "AndroidMulticastLock.h"

#include <tanh/core/AbiNamespace.h>
#include <tanh/link/AndroidLink.h>

#if defined(THL_PLATFORM_ANDROID)

#include <jni.h>
#include <tanh/core/Logger.h>

#include <mutex>

namespace thl::inline THL_ABI_NAMESPACE::link {
namespace {
constexpr const char* k_log_group = "thl.link.android";
JavaVM* g_java_vm = nullptr;

/// Attaches the calling thread for the scope when it is not attached yet.
struct ScopedJNIEnv {
    JNIEnv* m_env = nullptr;
    bool m_needs_detach = false;

    ScopedJNIEnv() {
        if (g_java_vm == nullptr) { return; }
        const jint status = g_java_vm->GetEnv(reinterpret_cast<void**>(&m_env), JNI_VERSION_1_6);
        if (status == JNI_EDETACHED) {
            if (g_java_vm->AttachCurrentThread(&m_env, nullptr) == JNI_OK) {
                m_needs_detach = true;
            } else {
                m_env = nullptr;
            }
        } else if (status != JNI_OK) {
            m_env = nullptr;
        }
    }
    ScopedJNIEnv(const ScopedJNIEnv&) = delete;
    ScopedJNIEnv& operator=(const ScopedJNIEnv&) = delete;
    ScopedJNIEnv(ScopedJNIEnv&&) = delete;
    ScopedJNIEnv& operator=(ScopedJNIEnv&&) = delete;
    ~ScopedJNIEnv() {
        if (m_needs_detach) { g_java_vm->DetachCurrentThread(); }
    }
};

/// Clears a pending Java exception (a SecurityException without the permission, say).
bool failed(JNIEnv* env, const char* what) {
    if (env->ExceptionCheck() == JNI_FALSE) { return false; }
    env->ExceptionClear();
    Logger::logf(Logger::LogLevel::Warning, k_log_group, "Java exception in %s", what);
    return true;
}

/// The application Context (local reference) via ActivityThread, as AudioIO and MIDI do.
jobject application_context(JNIEnv* env) {
    jclass activity_thread = env->FindClass("android/app/ActivityThread");
    if (failed(env, "FindClass(ActivityThread)") || activity_thread == nullptr) { return nullptr; }
    jmethodID current = env->GetStaticMethodID(activity_thread,
                                               "currentApplication",
                                               "()Landroid/app/Application;");
    jobject context = nullptr;
    if (!failed(env, "currentApplication lookup") && current != nullptr) {
        context = env->CallStaticObjectMethod(activity_thread, current);
        if (failed(env, "currentApplication")) { context = nullptr; }
    }
    env->DeleteLocalRef(activity_thread);
    return context;
}

/// A new WifiManager.MulticastLock (local reference), not reference counted.
jobject create_lock(JNIEnv* env) {
    jobject context = application_context(env);
    if (context == nullptr) { return nullptr; }
    jobject lock = nullptr;
    jclass context_class = env->FindClass("android/content/Context");
    jmethodID get_service = context_class == nullptr
                                ? nullptr
                                : env->GetMethodID(context_class,
                                                   "getSystemService",
                                                   "(Ljava/lang/String;)Ljava/lang/Object;");
    if (!failed(env, "getSystemService lookup") && get_service != nullptr) {
        jstring wifi_name = env->NewStringUTF("wifi");
        jobject wifi = env->CallObjectMethod(context, get_service, wifi_name);
        env->DeleteLocalRef(wifi_name);
        if (!failed(env, "getSystemService(wifi)") && wifi != nullptr) {
            jclass wifi_class = env->GetObjectClass(wifi);
            jmethodID create = env->GetMethodID(wifi_class,
                                                "createMulticastLock",
                                                "(Ljava/lang/String;)"
                                                "Landroid/net/wifi/WifiManager$MulticastLock;");
            if (!failed(env, "createMulticastLock lookup") && create != nullptr) {
                jstring tag = env->NewStringUTF("tanh-link");
                lock = env->CallObjectMethod(wifi, create, tag);
                env->DeleteLocalRef(tag);
                if (failed(env, "createMulticastLock")) { lock = nullptr; }
            }
            env->DeleteLocalRef(wifi_class);
            env->DeleteLocalRef(wifi);
        }
    }
    if (context_class != nullptr) { env->DeleteLocalRef(context_class); }
    env->DeleteLocalRef(context);
    return lock;
}

/// Calls a void method of the lock, with an optional boolean argument.
bool call(JNIEnv* env, jobject lock, const char* name, const char* signature, jboolean* arg) {
    jclass lock_class = env->GetObjectClass(lock);
    jmethodID method = env->GetMethodID(lock_class, name, signature);
    env->DeleteLocalRef(lock_class);
    if (failed(env, name) || method == nullptr) { return false; }
    if (arg != nullptr) {
        env->CallVoidMethod(lock, method, *arg);
    } else {
        env->CallVoidMethod(lock, method);
    }
    return !failed(env, name);
}
}  // namespace

void set_android_java_vm(void* java_vm) {
    g_java_vm = static_cast<JavaVM*>(java_vm);
}

namespace detail {

namespace {
std::mutex g_lock_mutex;
int g_lock_users = 0;      // sessions holding the lock
jobject g_lock = nullptr;  // global reference while held

void take_lock() {
    const ScopedJNIEnv scoped;
    JNIEnv* env = scoped.m_env;
    if (env == nullptr) {
        Logger::logf(Logger::LogLevel::Warning,
                     k_log_group,
                     "no JavaVM (thl::link::set_android_java_vm() not called): no multicast "
                     "lock, Link peers may not be found");
        return;
    }
    jobject lock = create_lock(env);
    if (lock == nullptr) {
        Logger::logf(Logger::LogLevel::Warning,
                     k_log_group,
                     "no Wi-Fi multicast lock (CHANGE_WIFI_MULTICAST_STATE declared?)");
        return;
    }
    jboolean counted = JNI_FALSE;
    call(env, lock, "setReferenceCounted", "(Z)V", &counted);
    if (call(env, lock, "acquire", "()V", nullptr)) { g_lock = env->NewGlobalRef(lock); }
    env->DeleteLocalRef(lock);
}

void drop_lock() {
    if (g_lock == nullptr) { return; }
    const ScopedJNIEnv scoped;
    JNIEnv* env = scoped.m_env;
    if (env != nullptr) {  // else the VM is gone: nothing to release
        call(env, g_lock, "release", "()V", nullptr);
        env->DeleteGlobalRef(g_lock);
    }
    g_lock = nullptr;
}
}  // namespace

void acquire_multicast_lock() {
    const std::lock_guard<std::mutex> guard(g_lock_mutex);
    if (g_lock_users++ == 0) { take_lock(); }
}

void release_multicast_lock() {
    const std::lock_guard<std::mutex> guard(g_lock_mutex);
    if (g_lock_users == 0) { return; }
    if (--g_lock_users == 0) { drop_lock(); }
}

}  // namespace detail
}  // namespace thl::inline THL_ABI_NAMESPACE::link

#else

namespace thl::inline THL_ABI_NAMESPACE::link {

void set_android_java_vm(void* /*java_vm*/) {}

namespace detail {
void acquire_multicast_lock() {}
void release_multicast_lock() {}
}  // namespace detail

}  // namespace thl::inline THL_ABI_NAMESPACE::link

#endif
