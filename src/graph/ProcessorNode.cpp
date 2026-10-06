#include <tanh/graph/ProcessorNode.h>

#include <algorithm>
#include <utility>

namespace thl::graph {

ProcessorNode::ProcessorNode(std::shared_ptr<thl::dsp::BaseProcessor> processor,
                             size_t num_channels,
                             Type type)
    : m_processor(std::move(processor)), m_num_channels(num_channels), m_type(type) {}

PortLayout ProcessorNode::ports() const {
    if (m_type == Type::Generator) { return {.inputs = {}, .outputs = {m_num_channels}}; }
    return {.inputs = {m_num_channels}, .outputs = {m_num_channels}};
}

void ProcessorNode::prepare(const ProcessSpec& spec) {
    m_processor->prepare(spec.sample_rate, spec.max_block_size, m_num_channels);
}

void ProcessorNode::process(const ProcessContext& context) TANH_NONBLOCKING_FUNCTION {
    thl::core::BufferView& output = context.outputs[0];

    for (size_t ch = 0; ch < m_num_channels; ++ch) {
        if (m_type == Type::Effect) {
            std::copy_n(context.inputs[0].get_read_pointer(ch),
                        context.num_frames,
                        output.get_write_pointer(ch));
        } else {
            std::fill_n(output.get_write_pointer(ch), context.num_frames, 0.0f);
        }
    }

    m_processor->process_modulated(output);
}

}  // namespace thl::graph
