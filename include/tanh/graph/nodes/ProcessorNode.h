#pragma once

#include <tanh/core/Exports.h>
#include <tanh/dsp/BaseProcessor.h>
#include <tanh/graph/Node.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace thl::graph::nodes {

class TANH_API ProcessorNode final : public Node {
public:
    enum class Type : uint8_t { Effect, Generator };  // Effect: input -> output; Generator: output only

    ProcessorNode(std::shared_ptr<thl::dsp::BaseProcessor> processor,
                  size_t num_channels,
                  Type type = Type::Effect);

    const thl::dsp::BaseProcessor* processor() const { return m_processor.get(); }

    PortLayout ports() const override;
    std::vector<const void*> exclusive_resources() const override { return {m_processor.get()}; }

private:
    void prepare(const ProcessSpec& spec) override;
    void process(const ProcessContext& context) TANH_NONBLOCKING_FUNCTION override;

    std::shared_ptr<thl::dsp::BaseProcessor> m_processor;
    size_t m_num_channels;
    Type m_type;
};

}  // namespace thl::graph::nodes
