#pragma once

#include <tanh/core/AbiNamespace.h>
#include <tanh/core/Exports.h>

namespace thl::inline THL_ABI_NAMESPACE::midi {

/// Hands the process's JavaVM to the Android MIDI backend and binds its Java half, the
/// class tanhlab.tanhlib.midi.MidiBridge (android/java/ in the tanh-lib sources, which
/// the app compiles in).
///
/// Call it from JNI_OnLoad — that is where the app's class loader is the current one, so
/// the class can be found — and before the first DeviceManager is constructed. Until then,
/// or when the class is not part of the app, DeviceManager::running() is false on Android
/// and no input is listed.
///
/// Mirrors thl::set_android_java_vm (AudioIO) and thl::net::set_android_java_vm (Net): the
/// components are independent, so a consumer using several calls each. `java_vm` is the
/// JavaVM*, passed as void* so that this header does not need <jni.h>. No-op on every
/// other platform, so callers need no #ifdef.
TANH_API void set_android_java_vm(void* java_vm);

}  // namespace thl::inline THL_ABI_NAMESPACE::midi
