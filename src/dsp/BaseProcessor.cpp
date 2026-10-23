#include <tanh/dsp/BaseProcessor.h>

#include <cstdint>
#include <span>

#include "tanh/core/BufferView.h"
#include "tanh/utils/RealtimeSanitizer.h"

namespace thl::dsp {

void BaseProcessor::process_modulated(const thl::core::BufferView& buffer)
    TANH_NONBLOCKING_FUNCTION {
    split_and_process(buffer, get_change_points());
}

// Caller-injected: uses externally supplied change points
void BaseProcessor::process_modulated(const thl::core::BufferView& buffer,
                                      std::span<const uint32_t> change_points)
    TANH_NONBLOCKING_FUNCTION {
    split_and_process(buffer, change_points);
}

void BaseProcessor::process_dry_modulated(const thl::core::BufferView& buffer)
    TANH_NONBLOCKING_FUNCTION {
    split_and_process(buffer, get_change_points(), Signal::Dry);
}

void BaseProcessor::process_dry_modulated(const thl::core::BufferView& buffer,
                                          std::span<const uint32_t> change_points)
    TANH_NONBLOCKING_FUNCTION {
    split_and_process(buffer, change_points, Signal::Dry);
}

void BaseProcessor::split_and_process(const thl::core::BufferView& buffer,
                                      std::span<const uint32_t> change_points,
                                      Signal signal) TANH_NONBLOCKING_FUNCTION {
    if (change_points.empty()) {
        signal == Signal::Dry ? process_dry(buffer, 0) : process(buffer, 0);
        return;
    }
    uint32_t pos = 0;
    const auto total = static_cast<uint32_t>(buffer.get_num_samples());
    for (uint32_t const cp : change_points) {
        if (cp <= pos || cp >= total) { continue; }
        const thl::core::BufferView part = buffer.sub_block(pos, cp - pos);
        signal == Signal::Dry ? process_dry(part, pos) : process(part, pos);
        pos = cp;
    }
    if (pos < total) {
        const thl::core::BufferView part = buffer.sub_block(pos, total - pos);
        signal == Signal::Dry ? process_dry(part, pos) : process(part, pos);
    }
}

}  // namespace thl::dsp
