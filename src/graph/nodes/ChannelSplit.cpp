#include <tanh/graph/nodes/ChannelSplit.h>

#include <algorithm>
#include <cassert>
#include <vector>

namespace thl::graph::nodes {

ChannelSplit::ChannelSplit(size_t num_channels) : m_num_channels(num_channels) {
    assert(num_channels > 0);
}

PortLayout ChannelSplit::ports() const {
    return {.inputs = {m_num_channels}, .outputs = std::vector<size_t>(m_num_channels, 1)};
}

void ChannelSplit::process(const ProcessContext& context) TANH_NONBLOCKING_FUNCTION {
    for (size_t ch = 0; ch < m_num_channels; ++ch) {
        std::copy_n(context.inputs[0].get_read_pointer(ch),
                    context.num_frames,
                    context.outputs[ch].get_write_pointer(0));
    }
}

}  // namespace thl::graph::nodes
