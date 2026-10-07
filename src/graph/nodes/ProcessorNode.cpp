#include <tanh/core/BufferView.h>
#include <tanh/dsp/BaseProcessor.h>
#include <tanh/graph/Node.h>
#include <tanh/graph/nodes/ProcessorNode.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <utility>

namespace thl::graph::nodes {

ProcessorNode::ProcessorNode(std::shared_ptr<thl::dsp::BaseProcessor> processor,
                             size_t num_channels,
                             Type type)
    : m_processor(std::move(processor)), m_num_channels(num_channels), m_type(type) {}

PortLayout ProcessorNode::ports() const {
    if (m_type == Type::Generator) { return {.m_inputs = {}, .m_outputs = {m_num_channels}}; }
    return {.m_inputs = {m_num_channels}, .m_outputs = {m_num_channels}};
}

void ProcessorNode::prepare(const ProcessSpec& spec) {
    m_processor->prepare(spec.m_sample_rate, spec.m_max_block_size, m_num_channels);
}

void ProcessorNode::process(const ProcessContext& context) TANH_NONBLOCKING_FUNCTION {
    thl::core::BufferView& output = context.m_outputs[0];

    for (size_t ch = 0; ch < m_num_channels; ++ch) {
        if (m_type == Type::Effect) {
            std::copy_n(context.m_inputs[0].get_read_pointer(ch),
                        context.m_num_frames,
                        output.get_write_pointer(ch));
        } else {
            std::fill_n(output.get_write_pointer(ch), context.m_num_frames, 0.0f);
        }
    }

    m_processor->process_modulated(output);
}

}  // namespace thl::graph::nodes
