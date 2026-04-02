#pragma once

#include <cstdint>
#include <span>
#include <tanh/utils/RealtimeSanitizer.h>
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
                         uint32_t modulation_offset = 0) TANH_NONBLOCKING_FUNCTION = 0;

    // Self-driven: calls get_change_points() to obtain split positions
    void process_modulated(thl::dsp::audio::AudioBufferView buffer) TANH_NONBLOCKING_FUNCTION;

    // Caller-injected: uses externally supplied change points
    void process_modulated(thl::dsp::audio::AudioBufferView buffer,
                           std::span<const uint32_t> change_points) TANH_NONBLOCKING_FUNCTION;

protected:
    virtual std::span<const uint32_t> get_change_points() TANH_NONBLOCKING_FUNCTION { return {}; }

private:
    void split_and_process(thl::dsp::audio::AudioBufferView buffer,
                           std::span<const uint32_t> change_points) TANH_NONBLOCKING_FUNCTION;
};

}  // namespace dsp
}  // namespace thl
