#pragma once

#include <tanh/core/AbiNamespace.h>

namespace thl::inline THL_ABI_NAMESPACE::link::detail {

/// A process-wide WifiManager.MulticastLock, held while any LinkSession is enabled on
/// Android (incoming multicast is filtered otherwise, so peers are not discovered).
/// Each acquire is matched by one release; the lock goes when the last one is released.
/// Thread-safe. No-ops elsewhere.
void acquire_multicast_lock();
void release_multicast_lock();

}  // namespace thl::inline THL_ABI_NAMESPACE::link::detail
