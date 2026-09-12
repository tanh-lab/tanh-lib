#pragma once
#include <tanh/core/Exports.h>

namespace thl::net {

/// Hand the Net component the process's JavaVM.
///
/// Android has no C HTTP API and the NDK ships no libcurl, so the backend goes
/// through java.net.HttpURLConnection over JNI — which needs a JavaVM to attach
/// to. Call this once during startup (JNI_OnLoad is the natural place) before
/// any transfer; until it is called, HttpClient::supported() is false on Android
/// and every request reports HttpStatus::Unsupported rather than failing oddly.
///
/// Mirrors thl::set_android_java_vm in the AudioIO component: the two are
/// independent, so a consumer using both calls both.
///
/// No-op on every other platform, so callers need no `#ifdef`.
TANH_API void set_android_java_vm(void* java_vm);

}  // namespace thl::net
