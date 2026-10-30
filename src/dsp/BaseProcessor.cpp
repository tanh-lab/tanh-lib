#include <tanh/dsp/BaseProcessor.h>

#include <cassert>
#include <cstdint>
#include <span>

#include "tanh/core/BufferOperations.h"
#include "tanh/core/BufferView.h"
#include "tanh/utils/RealtimeSanitizer.h"

namespace thl::dsp {

// NOLINTBEGIN(performance-unnecessary-value-param)
void BaseProcessor::process(thl::core::BufferView buffer,
                            uint32_t modulation_offset,
                            thl::core::BufferView dry) TANH_NONBLOCKING_FUNCTION {
    const bool with_dry = dry.get_num_channels() > 0;
    assert(!with_dry || (dry.get_num_channels() == buffer.get_num_channels() &&
                         dry.get_num_samples() == buffer.get_num_samples()));
    if (with_dry) { thl::core::BufferOperations::copy(dry, buffer); }
    process(buffer, modulation_offset);
}
// NOLINTEND(performance-unnecessary-value-param)

void BaseProcessor::process_modulated(const thl::core::BufferView& buffer,
                                      uint32_t modulation_offset) TANH_NONBLOCKING_FUNCTION {
    split_and_process(buffer, get_change_points(), modulation_offset);
}

void BaseProcessor::process_modulated(const thl::core::BufferView& buffer,
                                      const thl::core::BufferView& dry,
                                      uint32_t modulation_offset) TANH_NONBLOCKING_FUNCTION {
    split_and_process(buffer, get_change_points(), modulation_offset, dry);
}

// Caller-injected: uses externally supplied change points
void BaseProcessor::process_modulated(const thl::core::BufferView& buffer,
                                      std::span<const uint32_t> change_points,
                                      uint32_t modulation_offset) TANH_NONBLOCKING_FUNCTION {
    split_and_process(buffer, change_points, modulation_offset);
}

void BaseProcessor::process_modulated(const thl::core::BufferView& buffer,
                                      std::span<const uint32_t> change_points,
                                      const thl::core::BufferView& dry,
                                      uint32_t modulation_offset) TANH_NONBLOCKING_FUNCTION {
    split_and_process(buffer, change_points, modulation_offset, dry);
}

// @p buffer (and @p dry) hold block samples [begin, end); change points are block positions.
void BaseProcessor::split_and_process(const thl::core::BufferView& buffer,
                                      std::span<const uint32_t> change_points,
                                      uint32_t modulation_offset,
                                      const thl::core::BufferView& dry) TANH_NONBLOCKING_FUNCTION {
    const bool with_dry = dry.get_num_channels() > 0;
    assert(!with_dry || (dry.get_num_channels() == buffer.get_num_channels() &&
                         dry.get_num_samples() == buffer.get_num_samples()));
    const uint32_t begin = modulation_offset;
    const uint32_t end = begin + static_cast<uint32_t>(buffer.get_num_samples());
    const auto process_part = [&](uint32_t pos, uint32_t length) TANH_NONBLOCKING_FUNCTION {
        const uint32_t at = pos - begin;
        if (with_dry) {
            process(buffer.sub_block(at, length), pos, dry.sub_block(at, length));
        } else {
            process(buffer.sub_block(at, length), pos);
        }
    };

    uint32_t pos = begin;
    for (uint32_t const cp : change_points) {
        if (cp <= pos || cp >= end) { continue; }
        process_part(pos, cp - pos);
        pos = cp;
    }
    if (pos < end) { process_part(pos, end - pos); }
}

}  // namespace thl::dsp
