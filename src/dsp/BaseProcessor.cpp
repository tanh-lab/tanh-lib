#include <tanh/dsp/BaseProcessor.h>

namespace thl::dsp {

void BaseProcessor::process_modulated(thl::dsp::audio::AudioBufferView buffer) TANH_NONBLOCKING_FUNCTION {
    split_and_process(buffer, get_change_points());
}

// Caller-injected: uses externally supplied change points
void BaseProcessor::process_modulated(thl::dsp::audio::AudioBufferView buffer,
                        std::span<const uint32_t> change_points) TANH_NONBLOCKING_FUNCTION {
    split_and_process(buffer, change_points);
}

void BaseProcessor::split_and_process(thl::dsp::audio::AudioBufferView buffer,
                        std::span<const uint32_t> change_points) TANH_NONBLOCKING_FUNCTION {
    if (change_points.empty()) {
        process(buffer, 0);
        return;
    }
    uint32_t pos = 0;
    const auto total = static_cast<uint32_t>(buffer.get_num_frames());
    for (uint32_t cp : change_points) {
        if (cp <= pos || cp >= total) continue;
        process(buffer.sub_block(pos, cp - pos), pos);
        pos = cp;
    }
    if (pos < total) {
        process(buffer.sub_block(pos, total - pos), pos);
    }
}

}
