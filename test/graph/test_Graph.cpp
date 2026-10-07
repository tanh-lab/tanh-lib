#include <gtest/gtest.h>

#include <tanh/core/Buffer.h>
#include <tanh/core/BufferView.h>
#include <tanh/dsp/BaseProcessor.h>
#include <tanh/graph/Graph.h>
#include <tanh/graph/Node.h>
#include <tanh/graph/nodes/ChannelMerge.h>
#include <tanh/graph/nodes/ChannelSplit.h>
#include <tanh/graph/nodes/InputMix.h>
#include <tanh/graph/nodes/ProcessorNode.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>

using namespace thl::graph;
using namespace thl::graph::nodes;
using thl::core::Buffer;
using thl::core::BufferView;

namespace {

constexpr size_t k_channels = 2;
constexpr size_t k_block = 64;
constexpr ProcessSpec k_spec{.sample_rate = 48000.0, .max_block_size = k_block};
constexpr float k_untouched = -1.0f;

class ConstantNode final : public Node {
public:
    ConstantNode(size_t num_channels, float value) : m_num_channels(num_channels), m_value(value) {}

    PortLayout ports() const override { return {.inputs = {}, .outputs = {m_num_channels}}; }

private:
    void process(const ProcessContext& context) TANH_NONBLOCKING_FUNCTION override {
        for (size_t ch = 0; ch < m_num_channels; ++ch) {
            std::fill_n(context.outputs[0].get_write_pointer(ch), context.num_frames, m_value);
        }
    }

    size_t m_num_channels;
    float m_value;
};

class GainNode final : public Node {
public:
    GainNode(size_t num_channels, float gain) : m_num_channels(num_channels), m_gain(gain) {}

    PortLayout ports() const override {
        return {.inputs = {m_num_channels}, .outputs = {m_num_channels}};
    }

private:
    void process(const ProcessContext& context) TANH_NONBLOCKING_FUNCTION override {
        for (size_t ch = 0; ch < m_num_channels; ++ch) {
            const float* in = context.inputs[0].get_read_pointer(ch);
            float* out = context.outputs[0].get_write_pointer(ch);
            for (size_t i = 0; i < context.num_frames; ++i) { out[i] = in[i] * m_gain; }
        }
    }

    size_t m_num_channels;
    float m_gain;
};

class SumNode final : public Node {
public:
    explicit SumNode(size_t num_channels) : m_num_channels(num_channels) {}

    PortLayout ports() const override {
        return {.inputs = {m_num_channels, m_num_channels}, .outputs = {m_num_channels}};
    }

private:
    void process(const ProcessContext& context) TANH_NONBLOCKING_FUNCTION override {
        for (size_t ch = 0; ch < m_num_channels; ++ch) {
            const float* a = context.inputs[0].get_read_pointer(ch);
            const float* b = context.inputs[1].get_read_pointer(ch);
            float* out = context.outputs[0].get_write_pointer(ch);
            for (size_t i = 0; i < context.num_frames; ++i) { out[i] = a[i] + b[i]; }
        }
    }

    size_t m_num_channels;
};

/// Fills channel ch with ch + 1.
class ChannelNumberNode final : public Node {
public:
    explicit ChannelNumberNode(size_t num_channels) : m_num_channels(num_channels) {}

    PortLayout ports() const override { return {.inputs = {}, .outputs = {m_num_channels}}; }

private:
    void process(const ProcessContext& context) TANH_NONBLOCKING_FUNCTION override {
        for (size_t ch = 0; ch < m_num_channels; ++ch) {
            std::fill_n(context.outputs[0].get_write_pointer(ch),
                        context.num_frames,
                        static_cast<float>(ch + 1));
        }
    }

    size_t m_num_channels;
};

/// x -> 2x + 1, in place. Records what prepare() received.
class TestProcessor final : public thl::dsp::BaseProcessor {
public:
    void prepare(const double& sample_rate,
                 const size_t& samples_per_block,
                 const size_t& num_channels) override {
        prepared_sample_rate = sample_rate;
        prepared_block_size = samples_per_block;
        prepared_channels = num_channels;
    }

    void process(BufferView buffer, uint32_t /*modulation_offset*/)
        TANH_NONBLOCKING_FUNCTION override {
        for (size_t ch = 0; ch < buffer.get_num_channels(); ++ch) {
            float* samples = buffer.get_write_pointer(ch);
            for (size_t i = 0; i < buffer.get_num_samples(); ++i) {
                samples[i] = 2.0f * samples[i] + 1.0f;
            }
        }
    }

    double prepared_sample_rate = 0.0;
    size_t prepared_block_size = 0;
    size_t prepared_channels = 0;
};

Buffer<float> make_output(size_t num_frames = k_block) {
    Buffer<float> output(k_channels, num_frames);
    for (size_t ch = 0; ch < k_channels; ++ch) {
        std::fill_n(output.get_write_pointer(ch), num_frames, k_untouched);
    }
    return output;
}

void expect_channel(const Buffer<float>& buffer, size_t ch, float value) {
    for (size_t i = 0; i < buffer.get_num_samples(); ++i) {
        ASSERT_FLOAT_EQ(buffer.get_read_pointer(ch)[i], value)
            << "channel " << ch << ", frame " << i;
    }
}

void expect_all(const Buffer<float>& buffer, float value) {
    for (size_t ch = 0; ch < buffer.get_num_channels(); ++ch) { expect_channel(buffer, ch, value); }
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Processing
// ─────────────────────────────────────────────────────────────────────────────

TEST(Graph, SilentBeforePrepare) {
    Graph graph(k_channels);
    const auto source = graph.add_node<ConstantNode>(k_channels, 0.5f);
    ASSERT_TRUE(source);
    ASSERT_TRUE(graph.connect({*source, 0}, {graph.graph_output(), 0}));
    ASSERT_TRUE(graph.commit());

    auto output = make_output();
    graph.process(output);
    expect_all(output, 0.0f);
}

TEST(Graph, ChainProducesExpectedSamples) {
    Graph graph(k_channels);
    const auto source = graph.add_node<ConstantNode>(k_channels, 0.5f);
    const auto gain = graph.add_node<GainNode>(k_channels, 2.0f);
    ASSERT_TRUE(source && gain);
    ASSERT_TRUE(graph.connect({*source, 0}, {*gain, 0}));
    ASSERT_TRUE(graph.connect({*gain, 0}, {graph.graph_output(), 0}));
    graph.prepare(k_spec);

    auto output = make_output();
    graph.process(output);
    expect_all(output, 1.0f);

    auto short_output = make_output(k_block / 4);
    graph.process(short_output);
    expect_all(short_output, 1.0f);
}

TEST(Graph, FanOutFeedsEveryInput) {
    Graph graph(k_channels);
    const auto source = graph.add_node<ConstantNode>(k_channels, 0.25f);
    const auto gain_a = graph.add_node<GainNode>(k_channels, 2.0f);
    const auto gain_b = graph.add_node<GainNode>(k_channels, 4.0f);
    const auto sum = graph.add_node<SumNode>(k_channels);
    ASSERT_TRUE(source && gain_a && gain_b && sum);
    ASSERT_TRUE(graph.connect({*source, 0}, {*gain_a, 0}));
    ASSERT_TRUE(graph.connect({*source, 0}, {*gain_b, 0}));
    ASSERT_TRUE(graph.connect({*gain_a, 0}, {*sum, 0}));
    ASSERT_TRUE(graph.connect({*gain_b, 0}, {*sum, 1}));
    ASSERT_TRUE(graph.connect({*sum, 0}, {graph.graph_output(), 0}));
    graph.prepare(k_spec);

    auto output = make_output();
    graph.process(output);
    expect_all(output, 0.25f * 2.0f + 0.25f * 4.0f);
}

TEST(Graph, UnconnectedInputReadsZeros) {
    Graph graph(k_channels);
    const auto source = graph.add_node<ConstantNode>(k_channels, 0.75f);
    const auto sum = graph.add_node<SumNode>(k_channels);
    ASSERT_TRUE(source && sum);
    ASSERT_TRUE(graph.connect({*source, 0}, {*sum, 0}));
    ASSERT_TRUE(graph.connect({*sum, 0}, {graph.graph_output(), 0}));
    graph.prepare(k_spec);

    auto output = make_output();
    graph.process(output);
    expect_all(output, 0.75f);
}

TEST(Graph, UnconnectedOutputIsSilent) {
    Graph graph(k_channels);
    ASSERT_TRUE(graph.add_node<ConstantNode>(k_channels, 0.5f));
    graph.prepare(k_spec);

    auto output = make_output();
    graph.process(output);
    expect_all(output, 0.0f);
}

TEST(Graph, BlockLongerThanMaxIsSilent) {
    Graph graph(k_channels);
    const auto source = graph.add_node<ConstantNode>(k_channels, 0.5f);
    ASSERT_TRUE(source);
    ASSERT_TRUE(graph.connect({*source, 0}, {graph.graph_output(), 0}));
    graph.prepare(k_spec);

    auto output = make_output(k_block * 2);
    graph.process(output);
    expect_all(output, 0.0f);
}

// ─────────────────────────────────────────────────────────────────────────────
// Editing
// ─────────────────────────────────────────────────────────────────────────────

TEST(Graph, ConnectRejectsOccupiedInput) {
    Graph graph(k_channels);
    const auto a = graph.add_node<ConstantNode>(k_channels, 0.5f);
    const auto b = graph.add_node<ConstantNode>(k_channels, 0.25f);
    ASSERT_TRUE(a && b);
    const PortRef output{graph.graph_output(), 0};

    EXPECT_TRUE(graph.connect({*a, 0}, output));
    EXPECT_FALSE(graph.connect({*b, 0}, output));
    EXPECT_TRUE(graph.disconnect(output));
    EXPECT_TRUE(graph.connect({*b, 0}, output));
}

TEST(Graph, ConnectRejectsInvalidConnections) {
    Graph graph(k_channels);
    const auto mono = graph.add_node<ConstantNode>(1, 1.0f);
    const auto a = graph.add_node<GainNode>(k_channels, 1.0f);
    const auto b = graph.add_node<GainNode>(k_channels, 1.0f);
    ASSERT_TRUE(mono && a && b);
    const PortRef output{graph.graph_output(), 0};

    EXPECT_FALSE(graph.connect({*mono, 0}, output));        // channel mismatch
    EXPECT_FALSE(graph.connect({*a, 1}, output));           // no output port 1
    EXPECT_FALSE(graph.connect({*a, 0}, {*b, 1}));          // no input port 1
    EXPECT_FALSE(graph.connect({NodeId{999}, 0}, output));  // unknown node
    EXPECT_FALSE(graph.connect({*a, 0}, {*a, 0}));          // self-connection
    EXPECT_TRUE(graph.connect({*a, 0}, {*b, 0}));
    EXPECT_FALSE(graph.connect({*b, 0}, {*a, 0}));          // cycle
}

TEST(Graph, RemoveNodeDropsConnections) {
    Graph graph(k_channels);
    const auto source = graph.add_node<ConstantNode>(k_channels, 0.5f);
    ASSERT_TRUE(source);
    const PortRef output_port{graph.graph_output(), 0};
    ASSERT_TRUE(graph.connect({*source, 0}, output_port));
    graph.prepare(k_spec);

    EXPECT_FALSE(graph.remove_node(graph.graph_output()));
    EXPECT_TRUE(graph.remove_node(*source));
    EXPECT_FALSE(graph.remove_node(*source));
    ASSERT_TRUE(graph.commit());

    auto output = make_output();
    graph.process(output);
    expect_all(output, 0.0f);

    const auto replacement = graph.add_node<ConstantNode>(k_channels, 0.25f);
    ASSERT_TRUE(replacement);
    EXPECT_TRUE(graph.connect({*replacement, 0}, output_port));
}

TEST(Graph, AddNodeRejectsNullptr) {
    Graph graph(k_channels);
    EXPECT_FALSE(graph.add_node(nullptr));
}

TEST(Graph, SameNodeCanOnlyBeAddedOnce) {
    auto node = std::make_shared<ConstantNode>(k_channels, 0.5f);
    Graph graph(k_channels);

    EXPECT_TRUE(graph.add_node(node));
    EXPECT_FALSE(graph.add_node(node));
}

TEST(Graph, RemovedNodeCanBeAddedAgain) {
    auto node = std::make_shared<ConstantNode>(k_channels, 0.5f);
    Graph graph(k_channels);

    const auto first = graph.add_node(node);
    ASSERT_TRUE(first);
    EXPECT_TRUE(graph.remove_node(*first));
    EXPECT_TRUE(graph.add_node(node));
}

TEST(Graph, NodeLooksUpById) {
    auto node = std::make_shared<ConstantNode>(k_channels, 0.5f);
    Graph graph(k_channels);
    const auto id = graph.add_node(node);
    ASSERT_TRUE(id);

    EXPECT_EQ(graph.node(*id), node.get());
    EXPECT_EQ(graph.node(NodeId{12345}), nullptr);

    const Graph& read_only = graph;
    const Node* output = read_only.node(graph.graph_output());
    ASSERT_NE(output, nullptr);
    EXPECT_EQ(output->ports().inputs, std::vector<size_t>{k_channels});
}

TEST(Graph, NodesListsEveryNode) {
    Graph graph(k_channels);
    const auto first = graph.add_node<ConstantNode>(k_channels, 0.5f);
    const auto second = graph.add_node<ConstantNode>(k_channels, 0.25f);
    ASSERT_TRUE(first && second);

    EXPECT_EQ(graph.nodes(), (std::vector<NodeId>{graph.graph_output(), *first, *second}));

    ASSERT_TRUE(graph.remove_node(*first));
    EXPECT_EQ(graph.nodes(), (std::vector<NodeId>{graph.graph_output(), *second}));
}

TEST(Graph, ConnectionsListsEveryConnection) {
    Graph graph(k_channels);
    const auto source = graph.add_node<ConstantNode>(k_channels, 0.5f);
    const auto gain = graph.add_node<GainNode>(k_channels, 2.0f);
    ASSERT_TRUE(source && gain);
    const PortRef output_port{graph.graph_output(), 0};
    ASSERT_TRUE(graph.connect({*source, 0}, {*gain, 0}));
    ASSERT_TRUE(graph.connect({*gain, 0}, output_port));

    auto connections = graph.connections();
    std::vector<Connection> expected{{.from = {*source, 0}, .to = {*gain, 0}},
                                     {.from = {*gain, 0}, .to = output_port}};
    std::ranges::sort(connections);
    std::ranges::sort(expected);
    EXPECT_EQ(connections, expected);
}

TEST(Graph, SourceOfFindsWhatFeedsAnInput) {
    Graph graph(k_channels);
    const auto source = graph.add_node<ConstantNode>(k_channels, 0.5f);
    const auto sum = graph.add_node<SumNode>(k_channels);
    ASSERT_TRUE(source && sum);
    ASSERT_TRUE(graph.connect({*source, 0}, {*sum, 0}));

    EXPECT_EQ(graph.source_of({*sum, 0}), (PortRef{*source, 0}));
    EXPECT_EQ(graph.source_of({*sum, 1}), std::nullopt);

    ASSERT_TRUE(graph.disconnect({*sum, 0}));
    EXPECT_EQ(graph.source_of({*sum, 0}), std::nullopt);
}

TEST(Graph, HasUncommittedChangesTracksEdits) {
    Graph graph(k_channels);
    EXPECT_FALSE(graph.has_uncommitted_changes());

    const auto source = graph.add_node<ConstantNode>(k_channels, 0.5f);
    ASSERT_TRUE(source);
    EXPECT_TRUE(graph.has_uncommitted_changes());
    ASSERT_TRUE(graph.commit());
    EXPECT_FALSE(graph.has_uncommitted_changes());

    const PortRef output_port{graph.graph_output(), 0};
    EXPECT_FALSE(graph.connect({*source, 0}, {*source, 0}));
    EXPECT_FALSE(graph.has_uncommitted_changes());

    ASSERT_TRUE(graph.connect({*source, 0}, output_port));
    EXPECT_TRUE(graph.has_uncommitted_changes());
    ASSERT_TRUE(graph.commit());

    ASSERT_TRUE(graph.disconnect(output_port));
    EXPECT_TRUE(graph.has_uncommitted_changes());
    ASSERT_TRUE(graph.commit());

    ASSERT_TRUE(graph.remove_node(*source));
    EXPECT_TRUE(graph.has_uncommitted_changes());
    ASSERT_TRUE(graph.commit());
    EXPECT_FALSE(graph.has_uncommitted_changes());
}

TEST(Graph, PrepareCommitsPendingEdits) {
    Graph graph(k_channels);
    const auto source = graph.add_node<ConstantNode>(k_channels, 0.5f);
    ASSERT_TRUE(source);
    ASSERT_TRUE(graph.connect({*source, 0}, {graph.graph_output(), 0}));

    graph.prepare(k_spec);
    EXPECT_FALSE(graph.has_uncommitted_changes());

    auto output = make_output();
    graph.process(output);
    expect_all(output, 0.5f);
}

// ─────────────────────────────────────────────────────────────────────────────
// ProcessorNode
// ─────────────────────────────────────────────────────────────────────────────

TEST(ProcessorNode, EffectProcessesItsInput) {
    auto processor = std::make_shared<TestProcessor>();
    Graph graph(k_channels);
    const auto source = graph.add_node<ConstantNode>(k_channels, 0.5f);
    const auto effect = graph.add_node<ProcessorNode>(processor, k_channels);
    ASSERT_TRUE(source && effect);
    ASSERT_TRUE(graph.connect({*source, 0}, {*effect, 0}));
    ASSERT_TRUE(graph.connect({*effect, 0}, {graph.graph_output(), 0}));
    graph.prepare(k_spec);

    EXPECT_DOUBLE_EQ(processor->prepared_sample_rate, k_spec.sample_rate);
    EXPECT_EQ(processor->prepared_block_size, k_spec.max_block_size);
    EXPECT_EQ(processor->prepared_channels, k_channels);

    auto output = make_output();
    graph.process(output);
    expect_all(output, 2.0f * 0.5f + 1.0f);
}

TEST(ProcessorNode, GeneratorStartsFromSilence) {
    auto processor = std::make_shared<TestProcessor>();
    Graph graph(k_channels);
    const auto source = graph.add_node<ConstantNode>(k_channels, 0.5f);
    const auto generator =
        graph.add_node<ProcessorNode>(processor, k_channels, ProcessorNode::Type::Generator);
    ASSERT_TRUE(source && generator);
    EXPECT_FALSE(graph.connect({*source, 0}, {*generator, 0}));
    ASSERT_TRUE(graph.connect({*generator, 0}, {graph.graph_output(), 0}));
    graph.prepare(k_spec);

    auto output = make_output();
    graph.process(output);
    expect_all(output, 1.0f);
}

TEST(ProcessorNode, SameProcessorCanOnlyBeAddedOnce) {
    auto processor = std::make_shared<TestProcessor>();
    Graph graph(k_channels);

    const auto first = graph.add_node<ProcessorNode>(processor, k_channels);
    ASSERT_TRUE(first);
    EXPECT_FALSE(graph.add_node<ProcessorNode>(processor, k_channels));
    EXPECT_TRUE(graph.add_node<ProcessorNode>(std::make_shared<TestProcessor>(), k_channels));

    EXPECT_TRUE(graph.remove_node(*first));
    EXPECT_TRUE(graph.add_node<ProcessorNode>(processor, k_channels));
}

// ─────────────────────────────────────────────────────────────────────────────
// Routing nodes
// ─────────────────────────────────────────────────────────────────────────────

TEST(ChannelSplit, SendsEachChannelToItsOwnOutput) {
    constexpr size_t num_channels = 3;
    for (uint32_t port = 0; port < num_channels; ++port) {
        SCOPED_TRACE(port);
        Graph graph(1);
        const auto source = graph.add_node<ChannelNumberNode>(num_channels);
        const auto splitter = graph.add_node<ChannelSplit>(num_channels);
        ASSERT_TRUE(source && splitter);
        EXPECT_EQ(graph.node(*splitter)->ports().outputs, std::vector<size_t>(num_channels, 1));
        ASSERT_TRUE(graph.connect({*source, 0}, {*splitter, 0}));
        ASSERT_TRUE(graph.connect({*splitter, port}, {graph.graph_output(), 0}));
        graph.prepare(k_spec);

        Buffer<float> output(1, k_block);
        graph.process(output);
        expect_all(output, static_cast<float>(port + 1));
    }
}

TEST(InputMix, AveragesItsInputs) {
    Graph graph(k_channels);
    const auto a = graph.add_node<ConstantNode>(k_channels, 0.5f);
    const auto b = graph.add_node<ConstantNode>(k_channels, 0.25f);
    const auto mixer = graph.add_node<InputMix>(2, k_channels);
    ASSERT_TRUE(a && b && mixer);
    ASSERT_TRUE(graph.connect({*a, 0}, {*mixer, 0}));
    ASSERT_TRUE(graph.connect({*b, 0}, {*mixer, 1}));
    ASSERT_TRUE(graph.connect({*mixer, 0}, {graph.graph_output(), 0}));
    graph.prepare(k_spec);

    auto output = make_output();
    graph.process(output);
    expect_all(output, 0.375f);
}

TEST(InputMix, SumsItsInputs) {
    Graph graph(k_channels);
    const auto a = graph.add_node<ConstantNode>(k_channels, 0.5f);
    const auto b = graph.add_node<ConstantNode>(k_channels, 0.25f);
    const auto mixer = graph.add_node<InputMix>(2, k_channels, InputMix::Mode::Sum);
    ASSERT_TRUE(a && b && mixer);
    ASSERT_TRUE(graph.connect({*a, 0}, {*mixer, 0}));
    ASSERT_TRUE(graph.connect({*b, 0}, {*mixer, 1}));
    ASSERT_TRUE(graph.connect({*mixer, 0}, {graph.graph_output(), 0}));
    graph.prepare(k_spec);

    auto output = make_output();
    graph.process(output);
    expect_all(output, 0.75f);
}

TEST(InputMix, UnconnectedInputCountsAsSilence) {
    Graph graph(k_channels);
    const auto source = graph.add_node<ConstantNode>(k_channels, 0.6f);
    const auto mixer = graph.add_node<InputMix>(3, k_channels);
    ASSERT_TRUE(source && mixer);
    ASSERT_TRUE(graph.connect({*source, 0}, {*mixer, 1}));
    ASSERT_TRUE(graph.connect({*mixer, 0}, {graph.graph_output(), 0}));
    graph.prepare(k_spec);

    auto output = make_output();
    graph.process(output);
    expect_all(output, 0.2f);
}

TEST(ChannelMerge, SwapsChannelsWithSplit) {
    Graph graph(k_channels);
    const auto source = graph.add_node<ChannelNumberNode>(k_channels);
    const auto split = graph.add_node<ChannelSplit>(k_channels);
    const auto merge = graph.add_node<ChannelMerge>(k_channels);
    ASSERT_TRUE(source && split && merge);
    ASSERT_TRUE(graph.connect({*source, 0}, {*split, 0}));
    ASSERT_TRUE(graph.connect({*split, 0}, {*merge, 1}));
    ASSERT_TRUE(graph.connect({*split, 1}, {*merge, 0}));
    ASSERT_TRUE(graph.connect({*merge, 0}, {graph.graph_output(), 0}));
    graph.prepare(k_spec);

    auto output = make_output();
    graph.process(output);
    expect_channel(output, 0, 2.0f);
    expect_channel(output, 1, 1.0f);
}

TEST(ChannelMerge, MonoToStereo) {
    Graph graph(k_channels);
    const auto mono = graph.add_node<ConstantNode>(1, 0.5f);
    const auto merge = graph.add_node<ChannelMerge>(k_channels);
    ASSERT_TRUE(mono && merge);
    ASSERT_TRUE(graph.connect({*mono, 0}, {*merge, 0}));
    ASSERT_TRUE(graph.connect({*mono, 0}, {*merge, 1}));
    ASSERT_TRUE(graph.connect({*merge, 0}, {graph.graph_output(), 0}));
    graph.prepare(k_spec);

    auto output = make_output();
    graph.process(output);
    expect_all(output, 0.5f);
}
