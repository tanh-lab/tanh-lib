#pragma once

#include <tanh/utils/RealtimeSanitizer.h>

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
[[nodiscard]] constexpr uint32_t spread_offset(size_t i, size_t n, uint32_t block_size) noexcept
    TANH_NONBLOCKING_FUNCTION {
    if (n == 0) { return 0; }
    return static_cast<uint32_t>((i * static_cast<size_t>(block_size)) / n);
}

}  // namespace thl::modulation::detail
