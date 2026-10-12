#include <tanh/dsp/BaseProcessor.h>

#include <cstdint>
#include <span>

#include "tanh/core/BufferView.h"
#include "tanh/utils/RealtimeSanitizer.h"

namespace thl::dsp {

void BaseProcessor::process_modulated(const thl::core::BufferView& buffer,
                                      uint32_t modulation_offset) TANH_NONBLOCKING_FUNCTION {
    split_and_process(buffer, get_change_points(), modulation_offset);
}

// Caller-injected: uses externally supplied change points
void BaseProcessor::process_modulated(const thl::core::BufferView& buffer,
                                      std::span<const uint32_t> change_points,
                                      uint32_t modulation_offset) TANH_NONBLOCKING_FUNCTION {
    split_and_process(buffer, change_points, modulation_offset);
}

// @p buffer holds block samples [begin, end); change points are block positions.
void BaseProcessor::split_and_process(const thl::core::BufferView& buffer,
                                      std::span<const uint32_t> change_points,
                                      uint32_t modulation_offset) TANH_NONBLOCKING_FUNCTION {
    const uint32_t begin = modulation_offset;
    const uint32_t end = begin + static_cast<uint32_t>(buffer.get_num_samples());
    uint32_t pos = begin;
    for (uint32_t const cp : change_points) {
        if (cp <= pos || cp >= end) { continue; }
        process(buffer.sub_block(pos - begin, cp - pos), pos);
        pos = cp;
    }
    if (pos < end) { process(buffer.sub_block(pos - begin, end - pos), pos); }
}

}  // namespace thl::dsp
