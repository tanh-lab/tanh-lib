// SPDX-License-Identifier: Apache-2.0
#include "tanh/core/Clock.h"

#include <tanh/utils/RealtimeSanitizer.h>

#include <cstdint>

#if defined(THL_PLATFORM_MACOS) || defined(THL_PLATFORM_IOS)
#include <time.h>  // NOLINT(modernize-deprecated-headers): clock_gettime_nsec_np
#else
#include <chrono>
#endif

namespace thl::core {

int64_t uptime_ns() TANH_NONBLOCKING_FUNCTION {
#if defined(THL_PLATFORM_MACOS) || defined(THL_PLATFORM_IOS)
    // UIKit / AppKit event timestamps and AudioTimeStamp host times count
    // uptime; libc++'s steady_clock counts sleep too (CLOCK_MONOTONIC_RAW).
    return static_cast<int64_t>(
        clock_gettime_nsec_np(CLOCK_UPTIME_RAW));  // NOLINT(misc-include-cleaner)
#else
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
#endif
}

}  // namespace thl::core
