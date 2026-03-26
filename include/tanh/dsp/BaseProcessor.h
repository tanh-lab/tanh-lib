#pragma once

#include <cstdint>
#include <span>
#include <tanh/dsp/audio/AudioBufferView.h>

namespace thl {
namespace dsp {

class BaseProcessor {
public:
    virtual ~BaseProcessor() = default;

    virtual void prepare(const double& sample_rate,
                         const size_t& samples_per_block,
                         const size_t& num_channels) = 0;
    virtual void process(thl::dsp::audio::AudioBufferView buffer,
                         uint32_t modulation_offset = 0) = 0;

    // Self-driven: calls get_change_points() to obtain split positions
    void process_modulated(thl::dsp::audio::AudioBufferView buffer) {
        split_and_process(buffer, get_change_points());
    }

    // Caller-injected: uses externally supplied change points
    void process_modulated(thl::dsp::audio::AudioBufferView buffer,
                           std::span<const uint32_t> change_points) {
        split_and_process(buffer, change_points);
    }

protected:
    virtual std::span<const uint32_t> get_change_points() {
        return {};
    }

private:
    void split_and_process(thl::dsp::audio::AudioBufferView buffer,
                           std::span<const uint32_t> change_points) {
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
};

}  // namespace dsp
}  // namespace thl
