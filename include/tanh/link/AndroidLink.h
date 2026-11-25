#pragma once

#include <tanh/core/AbiNamespace.h>
#include <tanh/core/Exports.h>

namespace thl::inline THL_ABI_NAMESPACE::link {

/// Hands the Link component the process's JavaVM (a JavaVM*, as void* so this header
/// needs no <jni.h>). Call it once from JNI_OnLoad, before a LinkSession is enabled.
///
/// Android drops incoming multicast on Wi-Fi unless the app holds a
/// WifiManager.MulticastLock, and Link finds its peers by multicast: an enabled
/// LinkSession holds one (the app declares CHANGE_WIFI_MULTICAST_STATE). Without the
/// JavaVM, Link still runs but other peers may never be found.
///
/// Mirrors thl::set_android_java_vm (AudioIO), thl::net:: and thl::midi::: the
/// components are independent. No-op on every other platform.
TANH_API void set_android_java_vm(void* java_vm);

}  // namespace thl::inline THL_ABI_NAMESPACE::link
