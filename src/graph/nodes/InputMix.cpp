#include <tanh/graph/nodes/InputMix.h>

#include <algorithm>
#include <cassert>
#include <vector>

namespace thl::graph::nodes {

InputMix::InputMix(size_t num_inputs, size_t num_channels, Mode mode)
    : m_num_inputs(num_inputs), m_num_channels(num_channels), m_mode(mode) {
    assert(num_inputs > 0 && num_channels > 0);
}

PortLayout InputMix::ports() const {
    return {.inputs = std::vector<size_t>(m_num_inputs, m_num_channels),
            .outputs = {m_num_channels}};
}

void InputMix::process(const ProcessContext& context) TANH_NONBLOCKING_FUNCTION {
    const float scale = 1.0f / static_cast<float>(m_num_inputs);

    for (size_t ch = 0; ch < m_num_channels; ++ch) {
        float* out = context.outputs[0].get_write_pointer(ch);
        std::copy_n(context.inputs[0].get_read_pointer(ch), context.num_frames, out);

        for (size_t input = 1; input < m_num_inputs; ++input) {
            const float* in = context.inputs[input].get_read_pointer(ch);
            for (size_t i = 0; i < context.num_frames; ++i) { out[i] += in[i]; }
        }
        if (m_mode == Mode::Average) {
            for (size_t i = 0; i < context.num_frames; ++i) { out[i] *= scale; }
        }
    }
}

}  // namespace thl::graph::nodes
