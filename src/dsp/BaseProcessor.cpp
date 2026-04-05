#include <tanh/dsp/BaseProcessor.h>
#include "tanh/dsp/audio/AudioBufferView.h"
#include "tanh/utils/RealtimeSanitizer.h"
#include <span>
#include <cstdint>

namespace thl::dsp {

void BaseProcessor::process_modulated(const thl::dsp::audio::AudioBufferView& buffer)
    TANH_NONBLOCKING_FUNCTION {
    split_and_process(buffer, get_change_points());
}

// Caller-injected: uses externally supplied change points
void BaseProcessor::process_modulated(const thl::dsp::audio::AudioBufferView& buffer,
                                      std::span<const uint32_t> change_points)
    TANH_NONBLOCKING_FUNCTION {
    split_and_process(buffer, change_points);
}

void BaseProcessor::split_and_process(const thl::dsp::audio::AudioBufferView& buffer,
                                      std::span<const uint32_t> change_points)
    TANH_NONBLOCKING_FUNCTION {
    if (change_points.empty()) {
        process(buffer, 0);
        return;
    }
    uint32_t pos = 0;
    const auto total = static_cast<uint32_t>(buffer.get_num_frames());
    for (uint32_t const cp : change_points) {
        if (cp <= pos || cp >= total) { continue; }
        process(buffer.sub_block(pos, cp - pos), pos);
        pos = cp;
    }
    if (pos < total) { process(buffer.sub_block(pos, total - pos), pos); }
}

}  // namespace thl::dsp
