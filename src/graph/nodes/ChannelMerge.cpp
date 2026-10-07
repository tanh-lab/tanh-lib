#include <tanh/graph/Node.h>
#include <tanh/graph/nodes/ChannelMerge.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <vector>

namespace thl::graph::nodes {

ChannelMerge::ChannelMerge(size_t num_channels) : m_num_channels(num_channels) {
    assert(num_channels > 0);
}

PortLayout ChannelMerge::ports() const {
    return {.m_inputs = std::vector<size_t>(m_num_channels, 1), .m_outputs = {m_num_channels}};
}

void ChannelMerge::process(const ProcessContext& context) TANH_NONBLOCKING_FUNCTION {
    for (size_t ch = 0; ch < m_num_channels; ++ch) {
        std::copy_n(context.m_inputs[ch].get_read_pointer(0),
                    context.m_num_frames,
                    context.m_outputs[0].get_write_pointer(ch));
    }
}

}  // namespace thl::graph::nodes
