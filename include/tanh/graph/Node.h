#pragma once

#include <tanh/core/BufferView.h>
#include <tanh/core/Exports.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace thl::graph {

struct ProcessSpec {
    double m_sample_rate = 0.0;
    size_t m_max_block_size = 0;
};

struct PortLayout {
    std::vector<size_t> m_inputs;
    std::vector<size_t> m_outputs;
};

struct ProcessContext {
    std::span<const thl::core::ConstBufferView> m_inputs;
    std::span<thl::core::BufferView> m_outputs;
    size_t m_num_frames = 0;
};

class TANH_API Node {
public:
    virtual ~Node();

    virtual PortLayout ports() const = 0;

    virtual uint32_t latency_samples() const { return 0; }

    virtual std::vector<const void*> exclusive_resources() const { return {}; }

private:
    friend class Graph;

    virtual void prepare(const ProcessSpec& spec) { (void)spec; }

    virtual void process(const ProcessContext& context) TANH_NONBLOCKING_FUNCTION = 0;
};

}  // namespace thl::graph