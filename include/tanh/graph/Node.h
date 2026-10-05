#pragma once

#include <tanh/core/Exports.h>
#include <tanh/core/BufferView.h>
#include <tanh/utils/RealtimeSanitizer.h>


#include <cstddef>
#include <cstdint>
#include <vector>

namespace thl::graph {

struct ProcessSpec {
    double sample_rate = 0.0;
    size_t max_block_size = 0;
};
    
struct PortLayout {
    std::vector<size_t> inputs;
    std::vector<size_t> outputs;
};

struct ProcessContext {
    std::span<const thl::core::ConstBufferView> inputs;
    std::span<thl::core::BufferView> outputs;
    size_t num_frames = 0;
};

class TANH_API Node {
public:
    virtual ~Node() = default;

    virtual PortLayout ports() const = 0;

    virtual uint32_t latency_samples() const { return 0; }

    virtual void prepare(const ProcessSpec& spec) { (void)spec; }
 
    virtual void process(const ProcessContext& context) TANH_NONBLOCKING_FUNCTION = 0;
};

}  // namespace thl::graph