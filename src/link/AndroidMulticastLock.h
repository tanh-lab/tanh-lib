#pragma once

#include <tanh/core/AbiNamespace.h>

namespace thl::inline THL_ABI_NAMESPACE::link::detail {

/// A WifiManager.MulticastLock held while Link is enabled on Android (incoming
/// multicast is filtered otherwise, so peers are not discovered). No-op elsewhere.
/// Message thread.
class AndroidMulticastLock {
public:
    AndroidMulticastLock() = default;
    ~AndroidMulticastLock() { release(); }

    AndroidMulticastLock(const AndroidMulticastLock&) = delete;
    AndroidMulticastLock& operator=(const AndroidMulticastLock&) = delete;
    AndroidMulticastLock(AndroidMulticastLock&&) = delete;
    AndroidMulticastLock& operator=(AndroidMulticastLock&&) = delete;

    void acquire();
    void release();

private:
    void* m_lock = nullptr;  // jobject global reference (Android)
};

}  // namespace thl::inline THL_ABI_NAMESPACE::link::detail
