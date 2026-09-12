#if defined(THL_PLATFORM_ANDROID)

#include <jni.h>
#include <tanh/net/AndroidHttp.h>
#include <tanh/net/HttpClient.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <ios>
#include <string>
#include <vector>

#include "HttpBackend.h"

namespace thl::net {
namespace {
JavaVM* g_java_vm = nullptr;
}  // namespace

void set_android_java_vm(void* java_vm) {
    g_java_vm = static_cast<JavaVM*>(java_vm);
}
}  // namespace thl::net

namespace thl::net::detail {
namespace {

constexpr std::size_t k_read_chunk = std::size_t{64} * 1024;

/// Attaches the calling thread for the duration of a transfer. Transfers run on
/// a worker, so this is nearly always an attach rather than a lookup.
struct ScopedJNIEnv {
    JNIEnv* m_env = nullptr;
    bool m_needs_detach = false;

    explicit ScopedJNIEnv(JavaVM* vm) {
        if (vm == nullptr) { return; }
        const jint status = vm->GetEnv(reinterpret_cast<void**>(&m_env), JNI_VERSION_1_6);
        if (status == JNI_EDETACHED) {
            if (vm->AttachCurrentThread(&m_env, nullptr) == JNI_OK) {
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

    ~ScopedJNIEnv() {
        if (m_needs_detach && g_java_vm != nullptr) { g_java_vm->DetachCurrentThread(); }
    }

    explicit operator bool() const { return m_env != nullptr; }
};

/// A pending Java exception would poison every later JNI call, so each step
/// clears it and reports failure rather than continuing.
bool failed(JNIEnv* env) {
    if (env->ExceptionCheck() == JNI_TRUE) {
        env->ExceptionClear();
        return true;
    }
    return false;
}

/// Deletes a local reference on scope exit. The frame would drop them anyway,
/// but a transfer loop can outlive the default local-reference budget.
struct LocalRef {
    JNIEnv* m_env = nullptr;
    jobject m_object = nullptr;

    LocalRef(JNIEnv* env, jobject object) : m_env(env), m_object(object) {}
    LocalRef(const LocalRef&) = delete;
    LocalRef& operator=(const LocalRef&) = delete;
    ~LocalRef() {
        if (m_object != nullptr) { m_env->DeleteLocalRef(m_object); }
    }

    explicit operator bool() const { return m_object != nullptr; }
};

}  // namespace

HttpResult perform(const Request& request) {
    HttpResult result;

    ScopedJNIEnv scoped(g_java_vm);
    if (!scoped) {
        result.m_status = HttpStatus::Unsupported;
        result.m_message = "No JavaVM: call thl::net::set_android_java_vm() during startup.";
        return result;
    }
    JNIEnv* env = scoped.m_env;

    const LocalRef url_string(env, env->NewStringUTF(request.m_url.c_str()));
    if (!url_string || failed(env)) {
        result.m_status = HttpStatus::NetworkError;
        result.m_message = "Could not build the URL string.";
        return result;
    }

    const LocalRef url_class(env, env->FindClass("java/net/URL"));
    if (!url_class || failed(env)) {
        result.m_status = HttpStatus::Unsupported;
        result.m_message = "java.net.URL is unavailable.";
        return result;
    }
    const auto url_ctor = env->GetMethodID(static_cast<jclass>(url_class.m_object),
                                           "<init>",
                                           "(Ljava/lang/String;)V");
    const LocalRef url(
        env,
        env->NewObject(static_cast<jclass>(url_class.m_object), url_ctor, url_string.m_object));
    if (!url || failed(env)) {
        result.m_status = HttpStatus::NetworkError;
        result.m_message = "Malformed URL.";
        return result;
    }

    const auto open_connection = env->GetMethodID(static_cast<jclass>(url_class.m_object),
                                                  "openConnection",
                                                  "()Ljava/net/URLConnection;");
    const LocalRef connection(env, env->CallObjectMethod(url.m_object, open_connection));
    if (!connection || failed(env)) {
        result.m_status = HttpStatus::NetworkError;
        result.m_message = "Could not open the connection.";
        return result;
    }

    const LocalRef http_class(env, env->FindClass("java/net/HttpURLConnection"));
    if (!http_class || failed(env)) {
        result.m_status = HttpStatus::Unsupported;
        result.m_message = "java.net.HttpURLConnection is unavailable.";
        return result;
    }
    const auto http = static_cast<jclass>(http_class.m_object);

    if (request.m_timeout_seconds > 0) {
        const jint milliseconds = request.m_timeout_seconds * 1000;
        env->CallVoidMethod(connection.m_object,
                            env->GetMethodID(http, "setConnectTimeout", "(I)V"),
                            milliseconds);
        // Bounds a stall between reads, not the whole transfer.
        env->CallVoidMethod(connection.m_object,
                            env->GetMethodID(http, "setReadTimeout", "(I)V"),
                            milliseconds);
        failed(env);
    }

    const bool to_file = !request.m_destination.empty();
    std::uint64_t resume_from = to_file ? request.m_resume_from : 0;

    if (resume_from > 0) {
        const LocalRef name(env, env->NewStringUTF("Range"));
        const LocalRef value(
            env,
            env->NewStringUTF(("bytes=" + std::to_string(resume_from) + "-").c_str()));
        env->CallVoidMethod(
            connection.m_object,
            env->GetMethodID(http, "setRequestProperty", "(Ljava/lang/String;Ljava/lang/String;)V"),
            name.m_object,
            value.m_object);
        failed(env);
    }

    const jint status_code =
        env->CallIntMethod(connection.m_object, env->GetMethodID(http, "getResponseCode", "()I"));
    if (failed(env)) {
        result.m_status = HttpStatus::NetworkError;
        result.m_message = "Could not read the response status.";
        return result;
    }
    result.m_http_code = static_cast<int>(status_code);

    const auto disconnect = env->GetMethodID(http, "disconnect", "()V");
    const auto close_connection = [&] {
        env->CallVoidMethod(connection.m_object, disconnect);
        failed(env);
    };

    if (status_code < 200 || status_code >= 300) {
        close_connection();
        result.m_status = HttpStatus::HttpError;
        result.m_message = "Server returned HTTP " + std::to_string(status_code);
        return result;
    }

    // A 200 answering a Range request means the whole body is coming; the
    // partial file is not a prefix of it, so start over instead of appending.
    if (resume_from > 0 && status_code == 200) { resume_from = 0; }

    const jlong declared =
        env->CallLongMethod(connection.m_object,
                            env->GetMethodID(http, "getContentLengthLong", "()J"));
    failed(env);

    const LocalRef stream(
        env,
        env->CallObjectMethod(connection.m_object,
                              env->GetMethodID(http, "getInputStream", "()Ljava/io/InputStream;")));
    if (!stream || failed(env)) {
        close_connection();
        result.m_status = HttpStatus::NetworkError;
        result.m_message = "Could not open the response stream.";
        return result;
    }

    const LocalRef stream_class(env, env->FindClass("java/io/InputStream"));
    const auto read_method =
        env->GetMethodID(static_cast<jclass>(stream_class.m_object), "read", "([BII)I");
    const auto close_stream =
        env->GetMethodID(static_cast<jclass>(stream_class.m_object), "close", "()V");

    const LocalRef chunk(env, env->NewByteArray(static_cast<jsize>(k_read_chunk)));
    if (!chunk || failed(env)) {
        close_connection();
        result.m_status = HttpStatus::NetworkError;
        result.m_message = "Could not allocate the read buffer.";
        return result;
    }

    std::ofstream file;
    if (to_file) {
        file.open(request.m_destination,
                  resume_from > 0 ? (std::ios::binary | std::ios::app)
                                  : (std::ios::binary | std::ios::trunc));
        if (!file.is_open()) {
            close_connection();
            result.m_status = HttpStatus::FileError;
            result.m_message = "Could not open the destination for writing.";
            return result;
        }
    }

    std::vector<char> buffer(k_read_chunk);
    std::uint64_t received = 0;
    HttpStatus status = HttpStatus::Ok;
    std::string message;

    while (true) {
        if (request.m_cancelled != nullptr &&
            request.m_cancelled->load(std::memory_order_relaxed)) {
            status = HttpStatus::Cancelled;
            message = "Cancelled.";
            break;
        }

        const jint read = env->CallIntMethod(stream.m_object,
                                             read_method,
                                             chunk.m_object,
                                             0,
                                             static_cast<jint>(k_read_chunk));
        if (failed(env)) {
            status = HttpStatus::NetworkError;
            message = "Read failed.";
            break;
        }
        if (read < 0) { break; }  // end of stream
        if (read == 0) { continue; }

        env->GetByteArrayRegion(static_cast<jbyteArray>(chunk.m_object),
                                0,
                                read,
                                reinterpret_cast<jbyte*>(buffer.data()));
        if (failed(env)) {
            status = HttpStatus::NetworkError;
            message = "Could not copy the received data.";
            break;
        }

        if (to_file) {
            file.write(buffer.data(), static_cast<std::streamsize>(read));
            if (!file) {
                status = HttpStatus::FileError;
                message = "Could not write the received data.";
                break;
            }
        } else if (request.m_body != nullptr) {
            if (request.m_max_body_bytes > 0 &&
                request.m_body->size() + static_cast<std::size_t>(read) >
                    request.m_max_body_bytes) {
                status = HttpStatus::HttpError;
                message = "Response body exceeded the caller's limit.";
                break;
            }
            request.m_body->append(buffer.data(), static_cast<std::size_t>(read));
        }

        received += static_cast<std::uint64_t>(read);

        if (request.m_on_progress != nullptr && *request.m_on_progress) {
            HttpProgress progress;
            progress.m_downloaded = resume_from + received;
            progress.m_total =
                declared > 0 ? resume_from + static_cast<std::uint64_t>(declared) : 0;
            if (!(*request.m_on_progress)(progress)) {
                status = HttpStatus::Cancelled;
                message = "Cancelled.";
                break;
            }
        }
    }

    if (to_file) {
        file.flush();
        if (file.bad() && status == HttpStatus::Ok) {
            status = HttpStatus::FileError;
            message = "Could not write the received data.";
        }
        file.close();
    }

    env->CallVoidMethod(stream.m_object, close_stream);
    failed(env);
    close_connection();

    result.m_bytes_written = received;
    result.m_status = status;
    result.m_message = message;
    return result;
}

bool backend_supported() {
    return g_java_vm != nullptr;
}

}  // namespace thl::net::detail

#endif  // THL_PLATFORM_ANDROID
