#include <tanh/graph/Node.h>
#include <tanh/graph/nodes/InputMix.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <vector>

namespace thl::graph::nodes {

InputMix::InputMix(size_t num_inputs, size_t num_channels, Mode mode)
    : m_num_inputs(num_inputs), m_num_channels(num_channels), m_mode(mode) {
    assert(num_inputs > 0 && num_channels > 0);
}

PortLayout InputMix::ports() const {
    return {.m_inputs = std::vector<size_t>(m_num_inputs, m_num_channels),
            .m_outputs = {m_num_channels}};
}

void InputMix::process(const ProcessContext& context) TANH_NONBLOCKING_FUNCTION {
    const float scale = 1.0f / static_cast<float>(m_num_inputs);

    for (size_t ch = 0; ch < m_num_channels; ++ch) {
        float* out = context.m_outputs[0].get_write_pointer(ch);
        std::copy_n(context.m_inputs[0].get_read_pointer(ch), context.m_num_frames, out);

        for (size_t input = 1; input < m_num_inputs; ++input) {
            const float* in = context.m_inputs[input].get_read_pointer(ch);
            for (size_t i = 0; i < context.m_num_frames; ++i) { out[i] += in[i]; }
        }
        if (m_mode == Mode::Average) {
            for (size_t i = 0; i < context.m_num_frames; ++i) { out[i] *= scale; }
        }
    }
}

}  // namespace thl::graph::nodes
