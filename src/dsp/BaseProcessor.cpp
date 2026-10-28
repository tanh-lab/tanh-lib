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

void BaseProcessor::process_modulated(const thl::core::BufferView& buffer)
    TANH_NONBLOCKING_FUNCTION {
    split_and_process(buffer, get_change_points());
}

void BaseProcessor::process_modulated(const thl::core::BufferView& buffer,
                                      const thl::core::BufferView& dry) TANH_NONBLOCKING_FUNCTION {
    split_and_process(buffer, get_change_points(), dry);
}

// Caller-injected: uses externally supplied change points
void BaseProcessor::process_modulated(const thl::core::BufferView& buffer,
                                      std::span<const uint32_t> change_points)
    TANH_NONBLOCKING_FUNCTION {
    split_and_process(buffer, change_points);
}

void BaseProcessor::process_modulated(const thl::core::BufferView& buffer,
                                      std::span<const uint32_t> change_points,
                                      const thl::core::BufferView& dry) TANH_NONBLOCKING_FUNCTION {
    split_and_process(buffer, change_points, dry);
}

void BaseProcessor::split_and_process(const thl::core::BufferView& buffer,
                                      std::span<const uint32_t> change_points,
                                      const thl::core::BufferView& dry) TANH_NONBLOCKING_FUNCTION {
    const bool with_dry = dry.get_num_channels() > 0;
    assert(!with_dry || (dry.get_num_channels() == buffer.get_num_channels() &&
                         dry.get_num_samples() == buffer.get_num_samples()));
    const auto process_part = [&](uint32_t start, uint32_t length) {
        with_dry ? process(buffer.sub_block(start, length), start, dry.sub_block(start, length))
                 : process(buffer.sub_block(start, length), start);
    };

    const auto total = static_cast<uint32_t>(buffer.get_num_samples());
    if (change_points.empty()) {
        process_part(0, total);
        return;
    }
    uint32_t pos = 0;
    for (uint32_t const cp : change_points) {
        if (cp <= pos || cp >= total) { continue; }
        process_part(pos, cp - pos);
        pos = cp;
    }
    if (pos < total) { process_part(pos, total - pos); }
}

}  // namespace thl::dsp
