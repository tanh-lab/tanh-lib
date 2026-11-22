#pragma once

#include <tanh/core/AbiNamespace.h>

#include <cstddef>
#include <cstdint>

namespace thl::inline THL_ABI_NAMESPACE::modulation::detail {

/**
 * @brief In-block offset of the i-th of n untimestamped events of one stream.
 *
 * UI events carry no timestamp, so the events drained for one stream in one
 * block are spread evenly: event i of n lands at i * block_size / n. The first
 * event of every stream lands at offset 0, so simultaneous events on different
 * streams (a chord of touches) stay simultaneous. Offsets are monotonic in i
 * and always < block_size when block_size > 0.
 *
 * Used by InputEventQueue; any other queue that drains into a block places events the same way.
 */
[[nodiscard]] constexpr uint32_t spread_offset(size_t i, size_t n, uint32_t block_size) {
    if (n == 0) { return 0; }
    return static_cast<uint32_t>((i * static_cast<size_t>(block_size)) / n);
}

}  // namespace thl::inline THL_ABI_NAMESPACE::modulation::detail
