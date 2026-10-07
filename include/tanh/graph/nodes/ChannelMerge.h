#pragma once

#include <tanh/core/Exports.h>
#include <tanh/graph/Node.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <cstddef>

namespace thl::graph::nodes {

/// num_channels inputs with one channel each -> one output with num_channels channels.
class TANH_API ChannelMerge final : public Node {
public:
    explicit ChannelMerge(size_t num_channels);

    PortLayout ports() const override;

private:
    void process(const ProcessContext& context) TANH_NONBLOCKING_FUNCTION override;

    size_t m_num_channels;
};

}  // namespace thl::graph::nodes
