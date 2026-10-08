// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <tanh/core/Exports.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <cstdint>

namespace thl::core {

/**
 * @brief A monotonic clock in nanoseconds for input events and audio callbacks.
 *
 * On Apple platforms it is the uptime clock (mach_absolute_time): the clock of
 * UITouch and NSEvent timestamps and of AudioTimeStamp host times, so events and
 * callbacks compare directly. Elsewhere std::chrono::steady_clock. Real-time safe.
 */
[[nodiscard]] TANH_API int64_t uptime_ns() TANH_NONBLOCKING_FUNCTION;

}  // namespace thl::core
