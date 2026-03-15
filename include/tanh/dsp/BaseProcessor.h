#pragma once

#include <tanh/dsp/audio/AudioBufferView.h>

namespace thl {
namespace dsp {

class BaseProcessor {
public:
    virtual ~BaseProcessor() = default;

    virtual void prepare(const double& sample_rate,
                         const size_t& samples_per_block,
                         const size_t& num_channels) = 0;
    virtual void process(thl::dsp::audio::AudioBufferView buffer) = 0;
};

}  // namespace dsp
}  // namespace thl
