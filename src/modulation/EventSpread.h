#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace thl::modulation::detail {

/**
 * @brief In-block offset of the i-th of n untimestamped events of one stream.
 *
 * UI events carry no timestamp, so the events drained for one stream in one
 * block are spread evenly: event i of n lands at i * block_size / n. The first
 * event of every stream lands at offset 0, so simultaneous events on different
 * streams (a chord of touches) stay simultaneous. Offsets are monotonic in i
 * and always < block_size when block_size > 0.
 *
 * Shared by InputEventQueue and XYPad so both place events identically.
 */
[[nodiscard]] constexpr uint32_t spread_offset(size_t i, size_t n, uint32_t block_size) {
    if (n == 0) { return 0; }
    return static_cast<uint32_t>((i * static_cast<size_t>(block_size)) / n);
}

/**
 * @brief Offset, in samples from the block start, of an event stamped
 *        @p event_ns and played @p delay_ns late, when the block starts at
 *        @p block_ns on the same clock. Negative for an event already late.
 */
[[nodiscard]] inline int64_t timed_offset(int64_t event_ns,
                                          int64_t block_ns,
                                          int64_t delay_ns,
                                          double sample_rate) {
    const auto ns = static_cast<double>(event_ns - block_ns) + static_cast<double>(delay_ns);
    return static_cast<int64_t>(std::llround(ns * sample_rate * 1e-9));
}

}  // namespace thl::modulation::detail
