#include <tanh/graph/nodes/ChannelMerge.h>

#include <algorithm>
#include <cassert>
#include <vector>

namespace thl::graph::nodes {

ChannelMerge::ChannelMerge(size_t num_channels) : m_num_channels(num_channels) {
    assert(num_channels > 0);
}

PortLayout ChannelMerge::ports() const {
    return {.inputs = std::vector<size_t>(m_num_channels, 1), .outputs = {m_num_channels}};
}

void ChannelMerge::process(const ProcessContext& context) TANH_NONBLOCKING_FUNCTION {
    for (size_t ch = 0; ch < m_num_channels; ++ch) {
        std::copy_n(context.inputs[ch].get_read_pointer(0),
                    context.num_frames,
                    context.outputs[0].get_write_pointer(ch));
    }
}

}  // namespace thl::graph::nodes
