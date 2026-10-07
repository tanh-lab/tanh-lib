#pragma once

#include <tanh/core/Exports.h>
#include <tanh/graph/Node.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <cstddef>
#include <cstdint>

namespace thl::graph::nodes {

/// num_inputs inputs with num_channels channels each -> one output with num_channels channels.
/// Unconnected inputs count as silence, also when averaging.
class TANH_API InputMix final : public Node {
public:
    enum class Mode : uint8_t { Average, Sum };

    InputMix(size_t num_inputs, size_t num_channels, Mode mode = Mode::Average);

    PortLayout ports() const override;

private:
    void process(const ProcessContext& context) TANH_NONBLOCKING_FUNCTION override;

    size_t m_num_inputs;
    size_t m_num_channels;
    Mode m_mode;
};

}  // namespace thl::graph::nodes
