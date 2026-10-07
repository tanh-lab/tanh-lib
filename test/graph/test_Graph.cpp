#include <gtest/gtest.h>
#include <tanh/core/Buffer.h>
#include <tanh/core/BufferView.h>
#include <tanh/dsp/BaseProcessor.h>
#include <tanh/graph/Graph.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

using namespace thl::graph;
using thl::core::Buffer;
using thl::core::BufferView;
using thl::dsp::BaseProcessor;

namespace {

constexpr size_t k_channels = 2;
constexpr size_t k_block = 64;
constexpr double k_sample_rate = 48000.0;

/// Overwrites the buffer with a constant: a generator.
class ConstantProcessor final : public BaseProcessor {
public:
    explicit ConstantProcessor(float value) : m_value(value) {}

    void prepare(const double&, const size_t&, const size_t&) override {}

    void process(BufferView buffer,
                 uint32_t /*modulation_offset*/) TANH_NONBLOCKING_FUNCTION override {
        for (size_t ch = 0; ch < buffer.get_num_channels(); ++ch) {
            std::fill_n(buffer.get_write_pointer(ch), buffer.get_num_samples(), m_value);
        }
    }

private:
    float m_value;
};

class GainProcessor final : public BaseProcessor {
public:
    explicit GainProcessor(float gain) : m_gain(gain) {}

    void prepare(const double&, const size_t&, const size_t&) override {}

    void process(BufferView buffer,
                 uint32_t /*modulation_offset*/) TANH_NONBLOCKING_FUNCTION override {
        for (size_t ch = 0; ch < buffer.get_num_channels(); ++ch) {
            float* samples = buffer.get_write_pointer(ch);
            for (size_t i = 0; i < buffer.get_num_samples(); ++i) { samples[i] *= m_gain; }
        }
    }

private:
    float m_gain;
};

class PrepareRecorder final : public BaseProcessor {
public:
    void prepare(const double& sample_rate,
                 const size_t& samples_per_block,
                 const size_t& num_channels) override {
        m_sample_rate = sample_rate;
        m_samples_per_block = samples_per_block;
        m_num_channels = num_channels;
    }

    void process(BufferView, uint32_t) TANH_NONBLOCKING_FUNCTION override {}

    double m_sample_rate = 0.0;
    size_t m_samples_per_block = 0;
    size_t m_num_channels = 0;
};

/// Counts its own destruction in `destroyed`.
class LifetimeProcessor final : public BaseProcessor {
public:
    explicit LifetimeProcessor(int& destroyed) : m_destroyed(&destroyed) {}
    ~LifetimeProcessor() override { ++*m_destroyed; }

    LifetimeProcessor(const LifetimeProcessor&) = delete;
    LifetimeProcessor& operator=(const LifetimeProcessor&) = delete;
    LifetimeProcessor(LifetimeProcessor&&) = delete;
    LifetimeProcessor& operator=(LifetimeProcessor&&) = delete;

    void prepare(const double&, const size_t&, const size_t&) override {}
    void process(BufferView, uint32_t) TANH_NONBLOCKING_FUNCTION override {}

private:
    int* m_destroyed;
};

void prepare(Graph& graph) {
    graph.prepare(k_sample_rate, k_block, k_channels);
}

Buffer<float> make_buffer(float value, size_t num_frames = k_block) {
    Buffer<float> buffer(k_channels, num_frames);
    for (size_t ch = 0; ch < k_channels; ++ch) {
        std::fill_n(buffer.get_write_pointer(ch), num_frames, value);
    }
    return buffer;
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
    Graph graph;
    ASSERT_TRUE(graph.connect(graph.graph_input(), graph.graph_output()));

    auto buffer = make_buffer(0.5f);
    graph.process(buffer);
    expect_all(buffer, 0.0f);
}

TEST(Graph, InputPassesToOutput) {
    Graph graph;
    ASSERT_TRUE(graph.connect(graph.graph_input(), graph.graph_output()));
    prepare(graph);

    auto buffer = make_buffer(0.5f);
    graph.process(buffer);
    expect_all(buffer, 0.5f);
}

TEST(Graph, ChainProcessesInOrder) {
    Graph graph;
    const auto half = graph.add_node<GainProcessor>(0.5f);
    const auto triple = graph.add_node<GainProcessor>(3.0f);
    ASSERT_TRUE(half && triple);
    ASSERT_TRUE(graph.connect(graph.graph_input(), *half));
    ASSERT_TRUE(graph.connect(*half, *triple));
    ASSERT_TRUE(graph.connect(*triple, graph.graph_output()));
    prepare(graph);

    auto buffer = make_buffer(0.2f);
    graph.process(buffer);
    expect_all(buffer, 0.2f * 0.5f * 3.0f);
}

TEST(Graph, GeneratorWithoutSources) {
    Graph graph;
    const auto constant = graph.add_node<ConstantProcessor>(0.25f);
    ASSERT_TRUE(constant);
    ASSERT_TRUE(graph.connect(*constant, graph.graph_output()));
    prepare(graph);

    auto buffer = make_buffer(0.9f);
    graph.process(buffer);
    expect_all(buffer, 0.25f);
}

TEST(Graph, FanOutFeedsEveryNode) {
    Graph graph;
    const auto double_it = graph.add_node<GainProcessor>(2.0f);
    const auto halve_it = graph.add_node<GainProcessor>(0.5f);
    ASSERT_TRUE(double_it && halve_it);
    ASSERT_TRUE(graph.connect(graph.graph_input(), *double_it));
    ASSERT_TRUE(graph.connect(graph.graph_input(), *halve_it));
    ASSERT_TRUE(graph.connect(*double_it, graph.graph_output()));
    ASSERT_TRUE(graph.connect(*halve_it, graph.graph_output()));
    prepare(graph);

    auto buffer = make_buffer(0.4f);
    graph.process(buffer);
    expect_all(buffer, 0.4f * 2.0f + 0.4f * 0.5f);
}

TEST(Graph, MixModeSumsOrAveragesConnectedSources) {
    Graph graph;
    const auto a = graph.add_node<ConstantProcessor>(0.5f);
    const auto b = graph.add_node<ConstantProcessor>(0.25f);
    const auto unused = graph.add_node<ConstantProcessor>(1.0f);
    ASSERT_TRUE(a && b && unused);
    ASSERT_TRUE(graph.connect(*a, graph.graph_output()));
    ASSERT_TRUE(graph.connect(*b, graph.graph_output()));
    prepare(graph);

    auto buffer = make_buffer(0.0f);
    graph.process(buffer);
    expect_all(buffer, 0.75f);

    ASSERT_TRUE(graph.set_mix_mode(graph.graph_output(), Graph::MixMode::Average));
    ASSERT_TRUE(graph.commit());
    graph.process(buffer);
    expect_all(buffer, 0.375f);
}

TEST(Graph, MixModeAppliesToProcessorNodes) {
    Graph graph;
    const auto a = graph.add_node<ConstantProcessor>(0.5f);
    const auto b = graph.add_node<ConstantProcessor>(0.25f);
    const auto gain = graph.add_node<GainProcessor>(2.0f);
    ASSERT_TRUE(a && b && gain);
    ASSERT_TRUE(graph.connect(*a, *gain));
    ASSERT_TRUE(graph.connect(*b, *gain));
    ASSERT_TRUE(graph.connect(*gain, graph.graph_output()));
    ASSERT_TRUE(graph.set_mix_mode(*gain, Graph::MixMode::Average));
    prepare(graph);

    auto buffer = make_buffer(0.0f);
    graph.process(buffer);
    expect_all(buffer, 0.375f * 2.0f);
}

TEST(Graph, UnconnectedOutputIsSilent) {
    Graph graph;
    ASSERT_TRUE(graph.add_node<ConstantProcessor>(1.0f));
    prepare(graph);

    auto buffer = make_buffer(0.5f);
    graph.process(buffer);
    expect_all(buffer, 0.0f);
}

TEST(Graph, BufferNotMatchingPrepareIsSilent) {
    Graph graph;
    ASSERT_TRUE(graph.connect(graph.graph_input(), graph.graph_output()));
    prepare(graph);

    auto too_long = make_buffer(0.5f, k_block + 1);
    graph.process(too_long);
    expect_all(too_long, 0.0f);

    Buffer<float> mono(1, k_block);
    std::fill_n(mono.get_write_pointer(0), k_block, 0.5f);
    graph.process(mono);
    expect_all(mono, 0.0f);
}

// ─────────────────────────────────────────────────────────────────────────────
// Editing and inspection
// ─────────────────────────────────────────────────────────────────────────────

TEST(Graph, AddNodeRejectsNullptr) {
    Graph graph;
    EXPECT_FALSE(graph.add_node(nullptr));
}

TEST(Graph, ConnectRejectsInvalidConnections) {
    Graph graph;
    const auto a = graph.add_node<GainProcessor>(1.0f);
    const auto b = graph.add_node<GainProcessor>(1.0f);
    ASSERT_TRUE(a && b);

    EXPECT_FALSE(graph.connect(NodeId{999}, *a));           // unknown node
    EXPECT_FALSE(graph.connect(*a, *a));                    // self-connection
    EXPECT_FALSE(graph.connect(*a, graph.graph_input()));   // into the input
    EXPECT_FALSE(graph.connect(graph.graph_output(), *a));  // out of the output
    EXPECT_TRUE(graph.connect(*a, *b));
    EXPECT_FALSE(graph.connect(*a, *b));  // already connected
    EXPECT_FALSE(graph.connect(*b, *a));  // cycle
}

TEST(Graph, DisconnectRemovesOneConnection) {
    Graph graph;
    const auto a = graph.add_node<ConstantProcessor>(0.5f);
    const auto b = graph.add_node<ConstantProcessor>(0.25f);
    ASSERT_TRUE(a && b);
    ASSERT_TRUE(graph.connect(*a, graph.graph_output()));
    ASSERT_TRUE(graph.connect(*b, graph.graph_output()));

    EXPECT_TRUE(graph.disconnect(*a, graph.graph_output()));
    EXPECT_FALSE(graph.disconnect(*a, graph.graph_output()));
    EXPECT_EQ(graph.sources_of(graph.graph_output()), std::vector<NodeId>{*b});
}

TEST(Graph, RemoveNodeDropsConnections) {
    Graph graph;
    const auto gain = graph.add_node<GainProcessor>(2.0f);
    ASSERT_TRUE(gain);
    ASSERT_TRUE(graph.connect(graph.graph_input(), *gain));
    ASSERT_TRUE(graph.connect(*gain, graph.graph_output()));

    EXPECT_FALSE(graph.remove_node(graph.graph_input()));
    EXPECT_FALSE(graph.remove_node(graph.graph_output()));
    EXPECT_TRUE(graph.remove_node(*gain));
    EXPECT_FALSE(graph.remove_node(*gain));
    EXPECT_TRUE(graph.connections().empty());
}

TEST(Graph, NodeReturnsTheProcessor) {
    auto gain = std::make_unique<GainProcessor>(2.0f);
    const BaseProcessor* raw = gain.get();
    Graph graph;
    const auto id = graph.add_node(std::move(gain));
    ASSERT_TRUE(id);

    EXPECT_EQ(graph.node(*id), raw);
    EXPECT_EQ(graph.node(graph.graph_input()), nullptr);
    EXPECT_EQ(graph.node(graph.graph_output()), nullptr);
    EXPECT_EQ(graph.node(NodeId{999}), nullptr);
}

TEST(Graph, NodesAndConnectionsListTheGraph) {
    Graph graph;
    const auto gain = graph.add_node<GainProcessor>(2.0f);
    ASSERT_TRUE(gain);
    ASSERT_TRUE(graph.connect(graph.graph_input(), *gain));
    ASSERT_TRUE(graph.connect(*gain, graph.graph_output()));

    EXPECT_EQ(graph.nodes(),
              (std::vector<NodeId>{graph.graph_output(), graph.graph_input(), *gain}));

    auto connections = graph.connections();
    std::vector<Connection> expected{{.m_from = graph.graph_input(), .m_to = *gain},
                                     {.m_from = *gain, .m_to = graph.graph_output()}};
    std::ranges::sort(connections);
    std::ranges::sort(expected);
    EXPECT_EQ(connections, expected);

    EXPECT_EQ(graph.sources_of(*gain), std::vector<NodeId>{graph.graph_input()});
}

TEST(Graph, SetMixModeRejectsInputAndUnknownNodes) {
    Graph graph;
    EXPECT_FALSE(graph.set_mix_mode(graph.graph_input(), Graph::MixMode::Average));
    EXPECT_FALSE(graph.set_mix_mode(NodeId{999}, Graph::MixMode::Average));
    EXPECT_TRUE(graph.set_mix_mode(graph.graph_output(), Graph::MixMode::Average));
}

TEST(Graph, HasUncommittedChangesTracksEdits) {
    Graph graph;
    EXPECT_FALSE(graph.has_uncommitted_changes());

    const auto gain = graph.add_node<GainProcessor>(1.0f);
    ASSERT_TRUE(gain);
    EXPECT_TRUE(graph.has_uncommitted_changes());
    ASSERT_TRUE(graph.commit());
    EXPECT_FALSE(graph.has_uncommitted_changes());

    EXPECT_FALSE(graph.connect(*gain, *gain));
    EXPECT_FALSE(graph.has_uncommitted_changes());

    ASSERT_TRUE(graph.connect(*gain, graph.graph_output()));
    EXPECT_TRUE(graph.has_uncommitted_changes());
    ASSERT_TRUE(graph.commit());

    ASSERT_TRUE(graph.set_mix_mode(*gain, Graph::MixMode::Sum));  // unchanged
    EXPECT_FALSE(graph.has_uncommitted_changes());
    ASSERT_TRUE(graph.set_mix_mode(*gain, Graph::MixMode::Average));
    EXPECT_TRUE(graph.has_uncommitted_changes());
    ASSERT_TRUE(graph.commit());

    ASSERT_TRUE(graph.disconnect(*gain, graph.graph_output()));
    EXPECT_TRUE(graph.has_uncommitted_changes());
    ASSERT_TRUE(graph.commit());

    ASSERT_TRUE(graph.remove_node(*gain));
    EXPECT_TRUE(graph.has_uncommitted_changes());
    ASSERT_TRUE(graph.commit());
    EXPECT_FALSE(graph.has_uncommitted_changes());
}

// ─────────────────────────────────────────────────────────────────────────────
// Prepare
// ─────────────────────────────────────────────────────────────────────────────

TEST(Graph, PrepareReachesEveryProcessor) {
    auto before = std::make_unique<PrepareRecorder>();
    auto after = std::make_unique<PrepareRecorder>();
    const PrepareRecorder* added_before = before.get();
    const PrepareRecorder* added_after = after.get();

    Graph graph;
    ASSERT_TRUE(graph.add_node(std::move(before)));
    prepare(graph);
    ASSERT_TRUE(graph.add_node(std::move(after)));

    for (const PrepareRecorder* recorder : {added_before, added_after}) {
        EXPECT_DOUBLE_EQ(recorder->m_sample_rate, k_sample_rate);
        EXPECT_EQ(recorder->m_samples_per_block, k_block);
        EXPECT_EQ(recorder->m_num_channels, k_channels);
    }
}

TEST(Graph, PrepareCommitsPendingEdits) {
    Graph graph;
    ASSERT_TRUE(graph.connect(graph.graph_input(), graph.graph_output()));
    prepare(graph);
    EXPECT_FALSE(graph.has_uncommitted_changes());

    auto buffer = make_buffer(0.5f);
    graph.process(buffer);
    expect_all(buffer, 0.5f);
}

// ─────────────────────────────────────────────────────────────────────────────
// Handing versions to the audio thread
// ─────────────────────────────────────────────────────────────────────────────

TEST(Graph, CommitTakesEffectAtTheNextBlock) {
    Graph graph;
    prepare(graph);

    auto buffer = make_buffer(0.5f);
    graph.process(buffer);
    expect_all(buffer, 0.0f);

    ASSERT_TRUE(graph.connect(graph.graph_input(), graph.graph_output()));
    buffer = make_buffer(0.5f);
    graph.process(buffer);
    expect_all(buffer, 0.0f);  // not committed yet

    ASSERT_TRUE(graph.commit());
    buffer = make_buffer(0.5f);
    graph.process(buffer);
    expect_all(buffer, 0.5f);
}

TEST(Graph, NewestCommitWinsWhenSeveralArriveBetweenBlocks) {
    Graph graph;
    const auto a = graph.add_node<ConstantProcessor>(0.25f);
    const auto b = graph.add_node<ConstantProcessor>(0.75f);
    ASSERT_TRUE(a && b);
    prepare(graph);

    ASSERT_TRUE(graph.connect(*a, graph.graph_output()));
    ASSERT_TRUE(graph.commit());
    ASSERT_TRUE(graph.disconnect(*a, graph.graph_output()));
    ASSERT_TRUE(graph.connect(*b, graph.graph_output()));
    ASSERT_TRUE(graph.commit());

    auto buffer = make_buffer(0.0f);
    graph.process(buffer);
    expect_all(buffer, 0.75f);
}

TEST(Graph, RemovedNodeLivesUntilTheAudioThreadIsDoneWithIt) {
    int destroyed = 0;
    Graph graph;
    const auto node = graph.add_node<LifetimeProcessor>(destroyed);
    ASSERT_TRUE(node);
    ASSERT_TRUE(graph.connect(*node, graph.graph_output()));
    prepare(graph);

    ASSERT_TRUE(graph.remove_node(*node));
    ASSERT_TRUE(graph.commit());
    EXPECT_EQ(destroyed, 0);  // the audio thread has not switched yet

    auto buffer = make_buffer(0.0f);
    graph.process(buffer);    // switches and hands the old version back
    EXPECT_EQ(destroyed, 0);  // handed back, but not freed until the control thread collects it

    ASSERT_TRUE(graph.commit());
    EXPECT_EQ(destroyed, 1);
}

TEST(Graph, DestructorFreesEverything) {
    int destroyed = 0;
    {
        Graph graph;
        ASSERT_TRUE(graph.add_node<LifetimeProcessor>(destroyed));
        const auto removed = graph.add_node<LifetimeProcessor>(destroyed);
        ASSERT_TRUE(removed);
        prepare(graph);
        ASSERT_TRUE(graph.remove_node(*removed));
        ASSERT_TRUE(graph.commit());
    }
    EXPECT_EQ(destroyed, 2);
}
