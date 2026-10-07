#pragma once

#include <tanh/core/Exports.h>
#include <tanh/graph/Node.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <cstddef>

namespace thl::graph::nodes {

/// One input with num_channels channels -> num_channels outputs with one channel each.
class TANH_API ChannelSplit final : public Node {
public:
    explicit ChannelSplit(size_t num_channels);

    PortLayout ports() const override;

private:
    void process(const ProcessContext& context) TANH_NONBLOCKING_FUNCTION override;

    size_t m_num_channels;
};

}  // namespace thl::graph::nodes
