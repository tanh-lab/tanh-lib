#include <gtest/gtest.h>
#include <tanh/core/Buffer.h>
#include <tanh/core/BufferView.h>
#include <tanh/dsp/BaseProcessor.h>
#include <tanh/dsp/utils/DelayProcessor.h>
#include <tanh/dsp/utils/SmoothedValue.h>
#include <tanh/graph/Graph.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <random>
#include <span>
#include <thread>
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
constexpr size_t k_ramp_blocks = 15;     // 20 ms at 48 kHz, in blocks of 64
constexpr size_t k_fade_out_blocks = 8;  // 10 ms at 48 kHz, in blocks of 64, rounded up
constexpr size_t k_fade_blocks = 2 * k_fade_out_blocks + 1;  // out, in, then one at full level
constexpr uint32_t k_latency = 100;                          // longer than a block

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

/// Doubles the buffer and counts its calls in `calls`.
class CountingDoubler final : public BaseProcessor {
public:
    explicit CountingDoubler(int& calls) : m_calls(&calls) {}

    void prepare(const double&, const size_t&, const size_t&) override {}

    void process(BufferView buffer,
                 uint32_t /*modulation_offset*/) TANH_NONBLOCKING_FUNCTION override {
        ++*m_calls;
        for (size_t ch = 0; ch < buffer.get_num_channels(); ++ch) {
            float* samples = buffer.get_write_pointer(ch);
            for (size_t i = 0; i < buffer.get_num_samples(); ++i) { samples[i] *= 2.0f; }
        }
    }

private:
    int* m_calls;
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

/// Delays the buffer by `latency` samples and reports that as its latency.
class LatencyProcessor final : public BaseProcessor {
public:
    explicit LatencyProcessor(uint32_t latency) : LatencyProcessor(latency, latency) {}
    LatencyProcessor(uint32_t latency, uint32_t max_latency)
        : m_latency(latency), m_max_latency(max_latency), m_delay(max_latency) {}

    void prepare(const double& sample_rate,
                 const size_t& samples_per_block,
                 const size_t& num_channels) override {
        m_delay.prepare(sample_rate, samples_per_block, num_channels);
        m_delay.set_delay_samples(m_latency);
        set_max_latency_samples(m_max_latency);
        set_latency_samples(m_latency);
    }

    void process(BufferView buffer, uint32_t modulation_offset) TANH_NONBLOCKING_FUNCTION override {
        m_delay.process(buffer, modulation_offset);
    }

    void set_latency(uint32_t latency) {
        m_latency = latency;
        m_delay.set_delay_samples(latency);
        set_latency_samples(latency);
    }

private:
    uint32_t m_latency;
    uint32_t m_max_latency;
    thl::dsp::utils::DelayProcessor m_delay;
};

void prepare(Graph& graph) {
    graph.prepare(k_sample_rate, k_block, k_channels);
}

/// Every merge (the output included) averages its sources, so aligned parallel copies of a signal
/// give it back at its level: what the alignment tests measure.
void average_merges(Graph& graph) {
    for (const NodeId id : graph.nodes()) {
        (void)graph.set_input_mix(id, Graph::InputMix::Average);
    }
}

Buffer<float> make_buffer(float value, size_t num_frames = k_block) {
    Buffer<float> buffer(k_channels, num_frames);
    for (size_t ch = 0; ch < k_channels; ++ch) {
        std::fill_n(buffer.get_write_pointer(ch), num_frames, value);
    }
    return buffer;
}

/// Processes `num_blocks` blocks of `value` and returns the last one.
Buffer<float> process_blocks(Graph& graph, float value, size_t num_blocks) {
    auto buffer = make_buffer(value);
    for (size_t block = 0; block < num_blocks; ++block) {
        buffer = make_buffer(value);
        graph.process(buffer);
    }
    return buffer;
}

/// Feeds an impulse at frame 0 and expects a single 1.0 at `position` in the frames that follow.
void expect_impulse_response(Graph& graph, size_t position) {
    const size_t num_blocks = position / k_block + 1;
    for (size_t block = 0; block < num_blocks; ++block) {
        auto buffer = make_buffer(0.0f);
        if (block == 0) {
            for (size_t ch = 0; ch < k_channels; ++ch) { buffer.get_write_pointer(ch)[0] = 1.0f; }
        }
        graph.process(buffer);
        for (size_t ch = 0; ch < k_channels; ++ch) {
            for (size_t i = 0; i < k_block; ++i) {
                const size_t frame = block * k_block + i;
                ASSERT_FLOAT_EQ(buffer.get_read_pointer(ch)[i], frame == position ? 1.0f : 0.0f)
                    << "channel " << ch << ", frame " << frame;
            }
        }
    }
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
    expect_all(buffer, 0.4f * 2.0f + 0.4f * 0.5f);  // summed
}

TEST(Graph, InputMixSumsConnectedSourcesByDefaultOrAverages) {
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

    ASSERT_TRUE(graph.set_input_mix(graph.graph_output(), Graph::InputMix::Average));
    for (size_t block = 0; block <= k_ramp_blocks; ++block) { graph.process(buffer); }
    expect_all(buffer, 0.375f);
}

TEST(Graph, InputMixAppliesToProcessorNodes) {
    Graph graph;
    const auto a = graph.add_node<ConstantProcessor>(0.5f);
    const auto b = graph.add_node<ConstantProcessor>(0.25f);
    const auto gain = graph.add_node<GainProcessor>(2.0f);
    ASSERT_TRUE(a && b && gain);
    ASSERT_TRUE(graph.connect(*a, *gain));
    ASSERT_TRUE(graph.connect(*b, *gain));
    ASSERT_TRUE(graph.connect(*gain, graph.graph_output()));
    ASSERT_TRUE(graph.set_input_mix(*gain, Graph::InputMix::Sum));
    prepare(graph);

    auto buffer = make_buffer(0.0f);
    graph.process(buffer);
    expect_all(buffer, 0.75f * 2.0f);
}

TEST(Graph, InputMixChangeRampsWithoutCommit) {
    Graph graph;
    const auto a = graph.add_node<ConstantProcessor>(0.5f);
    const auto b = graph.add_node<ConstantProcessor>(0.25f);
    ASSERT_TRUE(a && b);
    ASSERT_TRUE(graph.connect(*a, graph.graph_output()));
    ASSERT_TRUE(graph.connect(*b, graph.graph_output()));
    prepare(graph);

    ASSERT_TRUE(graph.set_input_mix(graph.graph_output(), Graph::InputMix::Average));
    EXPECT_FALSE(graph.has_uncommitted_changes());

    auto buffer = make_buffer(0.0f);
    graph.process(buffer);
    for (size_t ch = 0; ch < k_channels; ++ch) {
        const float* samples = buffer.get_read_pointer(ch);
        EXPECT_LT(samples[0], 0.75f);  // from the sum, towards the average
        EXPECT_GT(samples[0], 0.745f);
        for (size_t i = 1; i < k_block; ++i) { EXPECT_LE(samples[i], samples[i - 1]); }
        EXPECT_GT(samples[k_block - 1], 0.375f);
    }
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
// Wet mix
// ─────────────────────────────────────────────────────────────────────────────

TEST(Graph, WetMixZeroSkipsTheProcessor) {
    int calls = 0;
    Graph graph;
    const auto node = graph.add_node<CountingDoubler>(calls);
    ASSERT_TRUE(node);
    ASSERT_TRUE(graph.connect(graph.graph_input(), *node));
    ASSERT_TRUE(graph.connect(*node, graph.graph_output()));
    ASSERT_TRUE(graph.set_wet_mix(*node, 0.0f));
    prepare(graph);

    auto buffer = make_buffer(0.5f);
    graph.process(buffer);
    expect_all(buffer, 0.5f);
    EXPECT_EQ(calls, 0);
}

TEST(Graph, WetMixBlendsDryAndProcessed) {
    Graph graph;
    const auto gain = graph.add_node<GainProcessor>(2.0f);
    ASSERT_TRUE(gain);
    ASSERT_TRUE(graph.connect(graph.graph_input(), *gain));
    ASSERT_TRUE(graph.connect(*gain, graph.graph_output()));
    ASSERT_TRUE(graph.set_wet_mix(*gain, 0.5f));
    prepare(graph);

    auto buffer = make_buffer(0.4f);
    graph.process(buffer);
    expect_all(buffer, 0.4f * 0.5f + 0.8f * 0.5f);
}

TEST(Graph, WetMixChangeRampsWithoutCommit) {
    Graph graph;
    const auto gain = graph.add_node<GainProcessor>(2.0f);
    ASSERT_TRUE(gain);
    ASSERT_TRUE(graph.connect(graph.graph_input(), *gain));
    ASSERT_TRUE(graph.connect(*gain, graph.graph_output()));
    prepare(graph);

    ASSERT_TRUE(graph.set_wet_mix(*gain, 0.0f));
    EXPECT_FALSE(graph.has_uncommitted_changes());

    auto buffer = make_buffer(0.5f);
    graph.process(buffer);
    for (size_t ch = 0; ch < k_channels; ++ch) {
        const float* samples = buffer.get_read_pointer(ch);
        EXPECT_LT(samples[0], 1.0f);
        EXPECT_GT(samples[0], 0.99f);
        for (size_t i = 1; i < k_block; ++i) { EXPECT_LE(samples[i], samples[i - 1]); }
        EXPECT_GT(samples[k_block - 1], 0.5f);
    }

    for (size_t block = 0; block < k_ramp_blocks; ++block) {
        buffer = make_buffer(0.5f);
        graph.process(buffer);
    }
    expect_all(buffer, 0.5f);
}

TEST(Graph, WetMixRampContinuesAcrossCommit) {
    Graph graph;
    Graph reference;
    for (Graph* g : {&graph, &reference}) {
        const auto gain = g->add_node<GainProcessor>(2.0f);
        ASSERT_TRUE(gain);
        ASSERT_TRUE(g->connect(g->graph_input(), *gain));
        ASSERT_TRUE(g->connect(*gain, g->graph_output()));
        prepare(*g);
        ASSERT_TRUE(g->set_wet_mix(*gain, 0.0f));
    }

    ASSERT_TRUE(graph.add_node<GainProcessor>(1.0f));  // an edit, so the commit switches
    ASSERT_TRUE(graph.commit());
    process_blocks(graph, 0.5f, k_fade_out_blocks);
    process_blocks(reference, 0.5f, k_fade_out_blocks);

    thl::dsp::utils::SmoothedValue fade_in;
    fade_in.reset(k_sample_rate, 0.01);
    fade_in.set_current_and_target_value(0.0f);
    fade_in.set_target_value(1.0f);

    auto buffer = make_buffer(0.5f);
    graph.process(buffer);
    auto expected = make_buffer(0.5f);
    reference.process(expected);
    for (size_t i = 0; i < k_block; ++i) {
        EXPECT_NEAR(buffer.get_read_pointer(0)[i],
                    expected.get_read_pointer(0)[i] * fade_in.get_smoothed_value(),
                    1e-6f);
    }
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

TEST(Graph, SetInputMixRejectsInputAndUnknownNodes) {
    Graph graph;
    EXPECT_FALSE(graph.set_input_mix(graph.graph_input(), Graph::InputMix::Average));
    EXPECT_FALSE(graph.set_input_mix(NodeId{999}, Graph::InputMix::Average));
    EXPECT_TRUE(graph.set_input_mix(graph.graph_output(), Graph::InputMix::Average));
}

TEST(Graph, SetWetMixClampsAndRejectsEndpointsUnknownNodesAndNaN) {
    Graph graph;
    const auto gain = graph.add_node<GainProcessor>(2.0f);
    ASSERT_TRUE(gain);
    EXPECT_EQ(graph.wet_mix(*gain), 1.0f);

    EXPECT_TRUE(graph.set_wet_mix(*gain, 1.5f));
    EXPECT_EQ(graph.wet_mix(*gain), 1.0f);
    EXPECT_TRUE(graph.set_wet_mix(*gain, -0.5f));
    EXPECT_EQ(graph.wet_mix(*gain), 0.0f);
    EXPECT_FALSE(graph.set_wet_mix(*gain, std::numeric_limits<float>::quiet_NaN()));
    EXPECT_EQ(graph.wet_mix(*gain), 0.0f);

    EXPECT_FALSE(graph.set_wet_mix(graph.graph_input(), 0.5f));
    EXPECT_FALSE(graph.set_wet_mix(graph.graph_output(), 0.5f));
    EXPECT_FALSE(graph.set_wet_mix(NodeId{999}, 0.5f));
    EXPECT_FALSE(graph.wet_mix(graph.graph_input()));
    EXPECT_FALSE(graph.wet_mix(graph.graph_output()));
    EXPECT_FALSE(graph.wet_mix(NodeId{999}));
}

TEST(Graph, WetMixBlendIsAlignedWithTheNodesLatency) {
    Graph graph;
    const auto latent = graph.add_node<LatencyProcessor>(k_latency);
    ASSERT_TRUE(latent);
    ASSERT_TRUE(graph.connect(graph.graph_input(), *latent));
    ASSERT_TRUE(graph.connect(*latent, graph.graph_output()));
    ASSERT_TRUE(graph.set_wet_mix(*latent, 0.5f));
    prepare(graph);

    expect_impulse_response(graph, k_latency);
}

TEST(Graph, BypassDelaysByTheNodesLatency) {
    Graph graph;
    const auto latent = graph.add_node<LatencyProcessor>(k_latency);
    ASSERT_TRUE(latent);
    ASSERT_TRUE(graph.connect(graph.graph_input(), *latent));
    ASSERT_TRUE(graph.connect(*latent, graph.graph_output()));
    ASSERT_TRUE(graph.set_wet_mix(*latent, 0.0f));
    prepare(graph);

    expect_impulse_response(graph, k_latency);
}

TEST(Graph, FullWetKeepsTheDryHistory) {
    Graph graph;
    const auto latent = graph.add_node<LatencyProcessor>(k_latency);
    ASSERT_TRUE(latent);
    ASSERT_TRUE(graph.connect(graph.graph_input(), *latent));
    ASSERT_TRUE(graph.connect(*latent, graph.graph_output()));
    prepare(graph);

    process_blocks(graph, 1.0f, k_latency / k_block + 1);  // past the latency
    ASSERT_TRUE(graph.set_wet_mix(*latent, 0.0f));
    for (size_t block = 0; block <= k_ramp_blocks; ++block) {
        expect_all(process_blocks(graph, 1.0f, 1), 1.0f);
    }
}

TEST(Graph, ParallelBranchIsAlignedWithALatentBranch) {
    Graph graph;
    const auto latent = graph.add_node<LatencyProcessor>(k_latency);
    ASSERT_TRUE(latent);
    ASSERT_TRUE(graph.connect(graph.graph_input(), *latent));
    ASSERT_TRUE(graph.connect(*latent, graph.graph_output()));
    ASSERT_TRUE(graph.connect(graph.graph_input(), graph.graph_output()));
    average_merges(graph);
    prepare(graph);

    expect_impulse_response(graph, k_latency);
}

TEST(Graph, MergeInsideTheGraphIsAligned) {
    Graph graph;
    const auto latent = graph.add_node<LatencyProcessor>(k_latency);
    const auto merge = graph.add_node<GainProcessor>(1.0f);
    ASSERT_TRUE(latent && merge);
    ASSERT_TRUE(graph.connect(graph.graph_input(), *latent));
    ASSERT_TRUE(graph.connect(*latent, *merge));
    ASSERT_TRUE(graph.connect(graph.graph_input(), *merge));
    ASSERT_TRUE(graph.connect(*merge, graph.graph_output()));
    average_merges(graph);
    prepare(graph);

    expect_impulse_response(graph, k_latency);
}

TEST(Graph, BranchesAreAlignedToTheSlowest) {
    Graph graph;
    const auto shorter = graph.add_node<LatencyProcessor>(40);
    const auto longer = graph.add_node<LatencyProcessor>(k_latency);
    ASSERT_TRUE(shorter && longer);
    ASSERT_TRUE(graph.connect(graph.graph_input(), *shorter));
    ASSERT_TRUE(graph.connect(graph.graph_input(), *longer));
    ASSERT_TRUE(graph.connect(*shorter, graph.graph_output()));
    ASSERT_TRUE(graph.connect(*longer, graph.graph_output()));
    ASSERT_TRUE(graph.connect(graph.graph_input(), graph.graph_output()));
    average_merges(graph);
    prepare(graph);

    expect_impulse_response(graph, k_latency);
}

TEST(Graph, ChainWithTheSameLatencyAsAParallelNodeIsAligned) {
    Graph graph;
    const auto first = graph.add_node<LatencyProcessor>(40);
    const auto second = graph.add_node<LatencyProcessor>(60);
    const auto parallel = graph.add_node<LatencyProcessor>(k_latency);
    ASSERT_TRUE(first && second && parallel);
    ASSERT_TRUE(graph.connect(graph.graph_input(), *first));
    ASSERT_TRUE(graph.connect(*first, *second));
    ASSERT_TRUE(graph.connect(*second, graph.graph_output()));
    ASSERT_TRUE(graph.connect(graph.graph_input(), *parallel));
    ASSERT_TRUE(graph.connect(*parallel, graph.graph_output()));
    average_merges(graph);
    prepare(graph);

    expect_impulse_response(graph, k_latency);
}

TEST(Graph, ParallelBranchFollowsALatencyChange) {
    Graph graph;
    auto processor = std::make_unique<LatencyProcessor>(k_latency, 2 * k_latency);
    LatencyProcessor* latent_processor = processor.get();
    const auto latent = graph.add_node(std::move(processor));
    ASSERT_TRUE(latent);
    ASSERT_TRUE(graph.connect(graph.graph_input(), *latent));
    ASSERT_TRUE(graph.connect(*latent, graph.graph_output()));
    ASSERT_TRUE(graph.connect(graph.graph_input(), graph.graph_output()));
    average_merges(graph);
    prepare(graph);

    process_blocks(graph, 0.0f, 2);
    latent_processor->set_latency(150);
    process_blocks(graph, 0.0f, 4);  // the crossfades finish

    expect_impulse_response(graph, 150);
}

TEST(Graph, DryDelayFollowsALatencyChange) {
    Graph graph;
    auto processor = std::make_unique<LatencyProcessor>(k_latency, 2 * k_latency);
    LatencyProcessor* latent_processor = processor.get();
    const auto latent = graph.add_node(std::move(processor));
    ASSERT_TRUE(latent);
    ASSERT_TRUE(graph.connect(graph.graph_input(), *latent));
    ASSERT_TRUE(graph.connect(*latent, graph.graph_output()));
    ASSERT_TRUE(graph.set_wet_mix(*latent, 0.5f));
    prepare(graph);

    process_blocks(graph, 0.0f, 2);
    latent_processor->set_latency(150);
    process_blocks(graph, 0.0f, 4);  // the crossfades finish

    expect_impulse_response(graph, 150);
}

TEST(Graph, CommitStaysSilentUntilTheNewLatencyHasPassed) {
    Graph graph;
    const auto latent = graph.add_node<LatencyProcessor>(k_latency);
    ASSERT_TRUE(latent);
    ASSERT_TRUE(graph.connect(graph.graph_input(), *latent));
    ASSERT_TRUE(graph.connect(*latent, graph.graph_output()));
    average_merges(graph);
    prepare(graph);
    process_blocks(graph, 1.0f, k_latency / k_block + 1);  // the latent node outputs 1.0

    ASSERT_TRUE(graph.connect(graph.graph_input(), graph.graph_output()));
    ASSERT_TRUE(graph.commit());

    std::vector<float> output;  // channel 0 over the fade-out, the hold and the fade-in
    for (size_t block = 0; block < k_fade_blocks + k_latency / k_block + 2; ++block) {
        const auto buffer = process_blocks(graph, 1.0f, 1);
        const float* samples = buffer.get_read_pointer(0);
        output.insert(output.end(), samples, samples + k_block);
    }

    size_t longest_silence = 0;
    size_t silence = 0;
    for (const float sample : output) {
        silence = sample == 0.0f ? silence + 1 : 0;
        longest_silence = std::max(longest_silence, silence);
    }
    EXPECT_GE(longest_silence, k_latency);
    EXPECT_FLOAT_EQ(output.back(), 1.0f);
}

TEST(Graph, ReportsTheLatencyOfItsSlowestPath) {
    Graph graph;
    const auto shorter = graph.add_node<LatencyProcessor>(40);
    const auto longer = graph.add_node<LatencyProcessor>(k_latency);
    ASSERT_TRUE(shorter && longer);
    ASSERT_TRUE(graph.connect(graph.graph_input(), *shorter));
    ASSERT_TRUE(graph.connect(graph.graph_input(), *longer));
    ASSERT_TRUE(graph.connect(*shorter, graph.graph_output()));
    ASSERT_TRUE(graph.connect(*longer, graph.graph_output()));
    prepare(graph);

    EXPECT_EQ(graph.latency_samples(), k_latency);
    EXPECT_EQ(graph.max_latency_samples(), k_latency);
}

TEST(Graph, ReportsTheSumOfAChain) {
    Graph graph;
    const auto first = graph.add_node<LatencyProcessor>(40);
    const auto second = graph.add_node<LatencyProcessor>(60);
    ASSERT_TRUE(first && second);
    ASSERT_TRUE(graph.connect(graph.graph_input(), *first));
    ASSERT_TRUE(graph.connect(*first, *second));
    ASSERT_TRUE(graph.connect(*second, graph.graph_output()));
    prepare(graph);

    EXPECT_EQ(graph.latency_samples(), 100u);
    EXPECT_EQ(graph.max_latency_samples(), 100u);
}

TEST(Graph, ReportsALatencyChangeAtTheNextBlock) {
    Graph graph;
    auto processor = std::make_unique<LatencyProcessor>(k_latency, 2 * k_latency);
    LatencyProcessor* latent_processor = processor.get();
    const auto latent = graph.add_node(std::move(processor));
    ASSERT_TRUE(latent);
    ASSERT_TRUE(graph.connect(graph.graph_input(), *latent));
    ASSERT_TRUE(graph.connect(*latent, graph.graph_output()));
    prepare(graph);
    EXPECT_EQ(graph.latency_samples(), k_latency);
    EXPECT_EQ(graph.max_latency_samples(), 2 * k_latency);

    latent_processor->set_latency(150);
    process_blocks(graph, 0.0f, 1);

    EXPECT_EQ(graph.latency_samples(), 150u);
    EXPECT_EQ(graph.max_latency_samples(), 2 * k_latency);
}

TEST(Graph, MaxLatencyOnlyRisesUntilPrepare) {
    Graph graph;
    const auto latent = graph.add_node<LatencyProcessor>(k_latency);
    ASSERT_TRUE(latent);
    ASSERT_TRUE(graph.connect(graph.graph_input(), *latent));
    ASSERT_TRUE(graph.connect(*latent, graph.graph_output()));
    prepare(graph);
    EXPECT_EQ(graph.max_latency_samples(), k_latency);

    const auto longer = graph.add_node<LatencyProcessor>(300);
    ASSERT_TRUE(longer);
    ASSERT_TRUE(graph.connect(graph.graph_input(), *longer));
    ASSERT_TRUE(graph.connect(*longer, graph.graph_output()));
    ASSERT_TRUE(graph.commit());
    EXPECT_EQ(graph.max_latency_samples(), 300u);

    ASSERT_TRUE(graph.remove_node(*longer));
    ASSERT_TRUE(graph.commit());
    EXPECT_EQ(graph.max_latency_samples(), 300u);

    prepare(graph);
    EXPECT_EQ(graph.max_latency_samples(), k_latency);
    EXPECT_EQ(graph.latency_samples(), k_latency);
}

TEST(Graph, CommittedLatencyIsKnownRightAfterCommit) {
    Graph graph;
    const auto latent = graph.add_node<LatencyProcessor>(k_latency);
    ASSERT_TRUE(latent);
    ASSERT_TRUE(graph.connect(graph.graph_input(), *latent));
    ASSERT_TRUE(graph.connect(*latent, graph.graph_output()));
    prepare(graph);
    EXPECT_EQ(graph.committed_latency_samples(), k_latency);

    const auto longer = graph.add_node<LatencyProcessor>(300);
    ASSERT_TRUE(longer);
    ASSERT_TRUE(graph.connect(graph.graph_input(), *longer));
    ASSERT_TRUE(graph.connect(*longer, graph.graph_output()));
    ASSERT_TRUE(graph.commit());
    EXPECT_EQ(graph.committed_latency_samples(), 300u);  // before the audio thread runs it
    EXPECT_EQ(graph.latency_samples(), k_latency);

    // Unlike the max latency, it falls again.
    ASSERT_TRUE(graph.remove_node(*longer));
    ASSERT_TRUE(graph.commit());
    EXPECT_EQ(graph.committed_latency_samples(), k_latency);
    EXPECT_EQ(graph.max_latency_samples(), 300u);
}

TEST(Graph, NestedGraphReportsItsLatencyToTheOuterGraph) {
    auto owned = std::make_unique<Graph>();
    Graph& inner = *owned;
    const auto latent = inner.add_node<LatencyProcessor>(k_latency);
    ASSERT_TRUE(latent);
    ASSERT_TRUE(inner.connect(inner.graph_input(), *latent));
    ASSERT_TRUE(inner.connect(*latent, inner.graph_output()));

    Graph outer;
    const auto nested = outer.add_node(std::move(owned));
    ASSERT_TRUE(nested);
    ASSERT_TRUE(outer.connect(outer.graph_input(), *nested));
    ASSERT_TRUE(outer.connect(*nested, outer.graph_output()));
    prepare(outer);

    EXPECT_EQ(outer.latency_samples(), k_latency);
    EXPECT_EQ(outer.max_latency_samples(), k_latency);
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

TEST(Graph, PrepareAgainTakesEffectAtTheNextBlock) {
    Graph graph;
    ASSERT_TRUE(graph.connect(graph.graph_input(), graph.graph_output()));
    prepare(graph);
    auto stereo = make_buffer(0.5f);
    graph.process(stereo);

    graph.prepare(k_sample_rate, k_block, 1);
    Buffer<float> mono(1, k_block);
    std::fill_n(mono.get_write_pointer(0), k_block, 0.5f);
    graph.process(mono);
    expect_all(mono, 0.5f);
}

// ─────────────────────────────────────────────────────────────────────────────
// Handing versions to the audio thread
// ─────────────────────────────────────────────────────────────────────────────

TEST(Graph, CommitTakesEffectAfterTheFade) {
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
    buffer = process_blocks(graph, 0.5f, k_fade_blocks);
    expect_all(buffer, 0.5f);
}

TEST(Graph, CommitFadesOutThenIn) {
    Graph graph;
    const auto gain = graph.add_node<GainProcessor>(2.0f);
    ASSERT_TRUE(gain);
    ASSERT_TRUE(graph.connect(graph.graph_input(), graph.graph_output()));
    prepare(graph);

    ASSERT_TRUE(graph.disconnect(graph.graph_input(), graph.graph_output()));
    ASSERT_TRUE(graph.connect(graph.graph_input(), *gain));
    ASSERT_TRUE(graph.connect(*gain, graph.graph_output()));
    ASSERT_TRUE(graph.commit());

    std::vector<float> output;
    for (size_t block = 0; block < k_fade_blocks; ++block) {
        auto buffer = make_buffer(0.5f);
        graph.process(buffer);
        const float* samples = buffer.get_read_pointer(0);
        output.insert(output.end(), samples, samples + k_block);
    }

    const size_t switch_at = k_fade_out_blocks * k_block;
    const auto first_silent = static_cast<size_t>(std::ranges::find(output, 0.0f) - output.begin());
    ASSERT_LT(first_silent, switch_at);
    EXPECT_LT(switch_at - first_silent, k_block);
    for (size_t i = first_silent; i < switch_at; ++i) { EXPECT_EQ(output[i], 0.0f); }
    EXPECT_GT(output[switch_at], 0.0f);

    for (size_t i = 1; i < output.size(); ++i) {
        if (i < switch_at) {
            EXPECT_LE(output[i], output[i - 1]);
        } else {
            EXPECT_GE(output[i], output[i - 1]);
        }
        EXPECT_LT(std::abs(output[i] - output[i - 1]), 0.0025f) << "jump at sample " << i;
    }
    EXPECT_FLOAT_EQ(output.back(), 1.0f);
}

TEST(Graph, FadeOutEndsWithABlockLongEnoughForIt) {
    constexpr size_t k_long_block = 1024;
    Graph graph;
    const auto gain = graph.add_node<GainProcessor>(2.0f);
    ASSERT_TRUE(gain);
    ASSERT_TRUE(graph.connect(graph.graph_input(), graph.graph_output()));
    graph.prepare(k_sample_rate, k_long_block, k_channels);

    ASSERT_TRUE(graph.disconnect(graph.graph_input(), graph.graph_output()));
    ASSERT_TRUE(graph.connect(graph.graph_input(), *gain));
    ASSERT_TRUE(graph.connect(*gain, graph.graph_output()));
    ASSERT_TRUE(graph.commit());

    auto buffer = make_buffer(0.5f, k_long_block);
    graph.process(buffer);
    const float* samples = buffer.get_read_pointer(0);
    EXPECT_FLOAT_EQ(samples[0], 0.5f);
    EXPECT_GT(samples[k_long_block - 3], 0.0f);
    EXPECT_EQ(samples[k_long_block - 1], 0.0f);

    buffer = make_buffer(0.5f, k_long_block);
    graph.process(buffer);
    samples = buffer.get_read_pointer(0);
    EXPECT_GT(samples[0], 0.0f);
    EXPECT_FLOAT_EQ(samples[k_long_block - 1], 1.0f);
}

TEST(Graph, CommitDuringFadeInFadesOutFromTheCurrentLevel) {
    Graph graph;
    const auto gain = graph.add_node<GainProcessor>(2.0f);
    ASSERT_TRUE(gain);
    prepare(graph);

    ASSERT_TRUE(graph.connect(graph.graph_input(), *gain));
    ASSERT_TRUE(graph.connect(*gain, graph.graph_output()));
    ASSERT_TRUE(graph.commit());
    auto buffer = process_blocks(graph, 0.5f, k_fade_out_blocks + 3);
    const float level = buffer.get_read_pointer(0)[k_block - 1];
    ASSERT_GT(level, 0.0f);
    ASSERT_LT(level, 1.0f);

    ASSERT_TRUE(graph.disconnect(*gain, graph.graph_output()));
    ASSERT_TRUE(graph.commit());
    buffer = make_buffer(0.5f);
    graph.process(buffer);
    const float* samples = buffer.get_read_pointer(0);
    EXPECT_LT(samples[0], level);
    EXPECT_NEAR(samples[0], level, 0.0025f);
    for (size_t i = 1; i < k_block; ++i) { EXPECT_LE(samples[i], samples[i - 1]); }
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

    const auto buffer = process_blocks(graph, 0.0f, k_fade_blocks);
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

    process_blocks(graph, 0.0f, k_fade_out_blocks);  // fades out, switches, hands the old one back
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

// ─────────────────────────────────────────────────────────────────────────────
// Nested graphs
// ─────────────────────────────────────────────────────────────────────────────

TEST(Graph, NestedGraphProcessesAsANode) {
    auto owned = std::make_unique<Graph>();
    Graph& inner = *owned;
    const auto inner_gain = inner.add_node<GainProcessor>(2.0f);
    ASSERT_TRUE(inner_gain);
    ASSERT_TRUE(inner.connect(inner.graph_input(), *inner_gain));
    ASSERT_TRUE(inner.connect(*inner_gain, inner.graph_output()));

    Graph outer;
    const auto nested = outer.add_node(std::move(owned));
    const auto outer_gain = outer.add_node<GainProcessor>(3.0f);
    ASSERT_TRUE(nested && outer_gain);
    ASSERT_TRUE(outer.connect(outer.graph_input(), *nested));
    ASSERT_TRUE(outer.connect(*nested, *outer_gain));
    ASSERT_TRUE(outer.connect(*outer_gain, outer.graph_output()));
    prepare(outer);

    auto buffer = make_buffer(0.1f);
    outer.process(buffer);
    expect_all(buffer, 0.1f * 2.0f * 3.0f);
}

TEST(Graph, PrepareReachesNestedGraphs) {
    auto before = std::make_unique<Graph>();
    auto after = std::make_unique<Graph>();
    const auto recorder_before = before->add_node<PrepareRecorder>();
    const auto recorder_after = after->add_node<PrepareRecorder>();
    ASSERT_TRUE(recorder_before && recorder_after);
    const auto* added_before = static_cast<const PrepareRecorder*>(before->node(*recorder_before));
    const auto* added_after = static_cast<const PrepareRecorder*>(after->node(*recorder_after));

    Graph outer;
    ASSERT_TRUE(outer.add_node(std::move(before)));
    prepare(outer);
    ASSERT_TRUE(outer.add_node(std::move(after)));

    for (const PrepareRecorder* recorder : {added_before, added_after}) {
        EXPECT_DOUBLE_EQ(recorder->m_sample_rate, k_sample_rate);
        EXPECT_EQ(recorder->m_samples_per_block, k_block);
        EXPECT_EQ(recorder->m_num_channels, k_channels);
    }
}

TEST(Graph, NestedCommitTakesEffectWithoutOuterCommit) {
    auto owned = std::make_unique<Graph>();
    Graph& inner = *owned;
    ASSERT_TRUE(inner.connect(inner.graph_input(), inner.graph_output()));

    Graph outer;
    const auto nested = outer.add_node(std::move(owned));
    ASSERT_TRUE(nested);
    ASSERT_TRUE(outer.connect(outer.graph_input(), *nested));
    ASSERT_TRUE(outer.connect(*nested, outer.graph_output()));
    prepare(outer);

    const auto gain = inner.add_node<GainProcessor>(2.0f);
    ASSERT_TRUE(gain);
    ASSERT_TRUE(inner.disconnect(inner.graph_input(), inner.graph_output()));
    ASSERT_TRUE(inner.connect(inner.graph_input(), *gain));
    ASSERT_TRUE(inner.connect(*gain, inner.graph_output()));
    ASSERT_TRUE(inner.commit());
    EXPECT_FALSE(outer.has_uncommitted_changes());

    const auto buffer = process_blocks(outer, 0.5f, k_fade_blocks);
    expect_all(buffer, 1.0f);
}

TEST(Graph, OuterCommitWithoutMaxChangeOnlyFadesTheNestedGraph) {
    auto owned = std::make_unique<Graph>();
    Graph& inner = *owned;
    ASSERT_TRUE(inner.connect(inner.graph_input(), inner.graph_output()));

    Graph outer;
    const auto nested = outer.add_node(std::move(owned));
    ASSERT_TRUE(nested);
    ASSERT_TRUE(outer.connect(outer.graph_input(), *nested));
    ASSERT_TRUE(outer.connect(*nested, outer.graph_output()));
    ASSERT_TRUE(outer.connect(outer.graph_input(), outer.graph_output()));
    average_merges(outer);
    prepare(outer);

    const auto gain = inner.add_node<GainProcessor>(2.0f);
    ASSERT_TRUE(gain);
    ASSERT_TRUE(inner.disconnect(inner.graph_input(), inner.graph_output()));
    ASSERT_TRUE(inner.connect(inner.graph_input(), *gain));
    ASSERT_TRUE(inner.connect(*gain, inner.graph_output()));
    ASSERT_TRUE(outer.commit());

    // The direct branch keeps its half of the average while only the nested graph fades.
    for (size_t block = 0; block < k_fade_blocks; ++block) {
        const auto buffer = process_blocks(outer, 1.0f, 1);
        for (size_t ch = 0; ch < k_channels; ++ch) {
            for (size_t i = 0; i < k_block; ++i) {
                ASSERT_GE(buffer.get_read_pointer(ch)[i], 0.5f);
            }
        }
    }
    expect_all(process_blocks(outer, 1.0f, 1), (1.0f + 2.0f) / 2.0f);
}

TEST(Graph, OuterCommitRealignsToARisingNestedMaxLatency) {
    auto owned = std::make_unique<Graph>();
    Graph& inner = *owned;
    ASSERT_TRUE(inner.connect(inner.graph_input(), inner.graph_output()));

    Graph outer;
    const auto nested = outer.add_node(std::move(owned));
    ASSERT_TRUE(nested);
    ASSERT_TRUE(outer.connect(outer.graph_input(), *nested));
    ASSERT_TRUE(outer.connect(*nested, outer.graph_output()));
    ASSERT_TRUE(outer.connect(outer.graph_input(), outer.graph_output()));
    average_merges(outer);
    prepare(outer);
    EXPECT_EQ(outer.max_latency_samples(), 0u);

    const auto latent = inner.add_node<LatencyProcessor>(k_latency);
    ASSERT_TRUE(latent);
    ASSERT_TRUE(inner.disconnect(inner.graph_input(), inner.graph_output()));
    ASSERT_TRUE(inner.connect(inner.graph_input(), *latent));
    ASSERT_TRUE(inner.connect(*latent, inner.graph_output()));
    ASSERT_TRUE(outer.commit());
    EXPECT_EQ(outer.max_latency_samples(), k_latency);

    process_blocks(outer, 0.0f, k_fade_blocks + 2);  // fades, crossfades, one block of lag
    expect_impulse_response(outer, k_latency);
}

TEST(Graph, OuterCommitReachesGraphsTwoLevelsDown) {
    auto owned_innermost = std::make_unique<Graph>();
    Graph& innermost = *owned_innermost;
    ASSERT_TRUE(innermost.connect(innermost.graph_input(), innermost.graph_output()));

    auto owned_middle = std::make_unique<Graph>();
    Graph& middle = *owned_middle;
    const auto middle_node = middle.add_node(std::move(owned_innermost));
    ASSERT_TRUE(middle_node);
    ASSERT_TRUE(middle.connect(middle.graph_input(), *middle_node));
    ASSERT_TRUE(middle.connect(*middle_node, middle.graph_output()));

    Graph outer;
    const auto outer_node = outer.add_node(std::move(owned_middle));
    ASSERT_TRUE(outer_node);
    ASSERT_TRUE(outer.connect(outer.graph_input(), *outer_node));
    ASSERT_TRUE(outer.connect(*outer_node, outer.graph_output()));
    prepare(outer);

    const auto gain = innermost.add_node<GainProcessor>(2.0f);
    ASSERT_TRUE(gain);
    ASSERT_TRUE(innermost.disconnect(innermost.graph_input(), innermost.graph_output()));
    ASSERT_TRUE(innermost.connect(innermost.graph_input(), *gain));
    ASSERT_TRUE(innermost.connect(*gain, innermost.graph_output()));
    ASSERT_TRUE(outer.commit());
    EXPECT_FALSE(innermost.has_uncommitted_changes());

    expect_all(process_blocks(outer, 0.5f, k_fade_blocks), 1.0f);
}

TEST(Graph, OuterGraphReportsUncommittedNestedChanges) {
    auto owned = std::make_unique<Graph>();
    Graph& inner = *owned;
    ASSERT_TRUE(inner.connect(inner.graph_input(), inner.graph_output()));

    Graph outer;
    const auto nested = outer.add_node(std::move(owned));
    ASSERT_TRUE(nested);
    ASSERT_TRUE(outer.connect(outer.graph_input(), *nested));
    ASSERT_TRUE(outer.connect(*nested, outer.graph_output()));
    prepare(outer);
    EXPECT_FALSE(outer.has_uncommitted_changes());

    const auto gain = inner.add_node<GainProcessor>(2.0f);
    ASSERT_TRUE(gain);
    EXPECT_TRUE(outer.has_uncommitted_changes());

    ASSERT_TRUE(outer.commit());
    EXPECT_FALSE(outer.has_uncommitted_changes());
}

TEST(Graph, NestedCommitWaitsWhileTheNestedGraphIsBypassed) {
    auto owned = std::make_unique<Graph>();
    Graph& inner = *owned;
    ASSERT_TRUE(inner.connect(inner.graph_input(), inner.graph_output()));

    Graph outer;
    const auto nested = outer.add_node(std::move(owned));
    ASSERT_TRUE(nested);
    ASSERT_TRUE(outer.connect(outer.graph_input(), *nested));
    ASSERT_TRUE(outer.connect(*nested, outer.graph_output()));
    ASSERT_TRUE(outer.set_wet_mix(*nested, 0.0f));
    prepare(outer);

    const auto gain = inner.add_node<GainProcessor>(2.0f);
    ASSERT_TRUE(gain);
    ASSERT_TRUE(inner.disconnect(inner.graph_input(), inner.graph_output()));
    ASSERT_TRUE(inner.connect(inner.graph_input(), *gain));
    ASSERT_TRUE(inner.connect(*gain, inner.graph_output()));
    ASSERT_TRUE(inner.commit());

    auto buffer = process_blocks(outer, 0.5f, k_fade_blocks);
    expect_all(buffer, 0.5f);

    // Running again, the nested graph starts by fading out its old version.
    ASSERT_TRUE(outer.set_wet_mix(*nested, 1.0f));
    buffer = process_blocks(outer, 0.5f, 1);
    EXPECT_LT(buffer.get_read_pointer(0)[k_block - 1], 0.5f);

    buffer = process_blocks(outer, 0.5f, k_ramp_blocks + k_fade_blocks);
    expect_all(buffer, 1.0f);
}

TEST(Graph, RemovedNestedGraphFreesItsNodes) {
    int destroyed = 0;
    auto owned = std::make_unique<Graph>();
    const auto lifetime = owned->add_node<LifetimeProcessor>(destroyed);
    ASSERT_TRUE(lifetime);
    ASSERT_TRUE(owned->connect(*lifetime, owned->graph_output()));

    Graph outer;
    const auto nested = outer.add_node(std::move(owned));
    ASSERT_TRUE(nested);
    ASSERT_TRUE(outer.connect(*nested, outer.graph_output()));
    prepare(outer);

    ASSERT_TRUE(outer.remove_node(*nested));
    ASSERT_TRUE(outer.commit());
    process_blocks(outer, 0.0f, k_fade_out_blocks);
    EXPECT_EQ(destroyed, 0);

    ASSERT_TRUE(outer.commit());
    EXPECT_EQ(destroyed, 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// Modulation offset
// ─────────────────────────────────────────────────────────────────────────────

namespace {

/// Has change points at 16 and 48 of a 64-sample block and records the
/// (modulation_offset, length) of every process() call.
class OffsetRecorder final : public BaseProcessor {
public:
    void prepare(const double&, const size_t&, const size_t&) override {}

    // Fixed storage: process() runs under RTSan.
    void process(BufferView buffer, uint32_t modulation_offset) TANH_NONBLOCKING_FUNCTION override {
        if (m_num_calls < m_storage.size()) {
            m_storage[m_num_calls++] = {modulation_offset,
                                        static_cast<uint32_t>(buffer.get_num_samples())};
        }
    }

    [[nodiscard]] std::vector<std::pair<uint32_t, uint32_t>> calls() const {
        return {m_storage.begin(), m_storage.begin() + static_cast<std::ptrdiff_t>(m_num_calls)};
    }

protected:
    std::span<const uint32_t> get_change_points() TANH_NONBLOCKING_FUNCTION override {
        return m_change_points;
    }

private:
    std::array<uint32_t, 2> m_change_points{16, 48};
    std::array<std::pair<uint32_t, uint32_t>, 16> m_storage{};
    size_t m_num_calls = 0;
};

using Calls = std::vector<std::pair<uint32_t, uint32_t>>;

}  // namespace

// A host that splits the graph's block passes each part's position; every node
// reads its modulation (and splits at its change points) at the block position.
TEST(Graph, NodesGetTheModulationOffsetOfASplitBlock) {
    auto owned = std::make_unique<OffsetRecorder>();
    OffsetRecorder& recorder = *owned;
    Graph graph;
    const auto node = graph.add_node(std::move(owned));
    ASSERT_TRUE(node);
    ASSERT_TRUE(graph.connect(graph.graph_input(), *node));
    ASSERT_TRUE(graph.connect(*node, graph.graph_output()));
    prepare(graph);

    auto buffer = make_buffer(0.5f);
    const BufferView view(buffer.get_array_of_write_pointers(), k_channels, k_block);
    graph.process(view.sub_block(0, 32), 0);
    graph.process(view.sub_block(32, 32), 32);
    EXPECT_EQ(recorder.calls(), (Calls{{0, 16}, {16, 16}, {32, 16}, {48, 16}}));
}

// The same through a nested graph: the outer graph passes its offset on.
TEST(Graph, NestedGraphNodesGetTheModulationOffset) {
    auto owned_recorder = std::make_unique<OffsetRecorder>();
    OffsetRecorder& recorder = *owned_recorder;
    auto inner = std::make_unique<Graph>();
    const auto node = inner->add_node(std::move(owned_recorder));
    ASSERT_TRUE(node);
    ASSERT_TRUE(inner->connect(inner->graph_input(), *node));
    ASSERT_TRUE(inner->connect(*node, inner->graph_output()));

    Graph outer;
    const auto nested = outer.add_node(std::move(inner));
    ASSERT_TRUE(nested);
    ASSERT_TRUE(outer.connect(outer.graph_input(), *nested));
    ASSERT_TRUE(outer.connect(*nested, outer.graph_output()));
    prepare(outer);

    auto buffer = make_buffer(0.5f);
    const BufferView view(buffer.get_array_of_write_pointers(), k_channels, k_block);
    outer.process(view.sub_block(0, 40), 0);
    outer.process(view.sub_block(40, 24), 40);
    EXPECT_EQ(recorder.calls(), (Calls{{0, 16}, {16, 24}, {40, 8}, {48, 16}}));
}

// ─────────────────────────────────────────────────────────────────────────────
// Control-thread cost: no-op commits, garbage, prepared nodes, buffer reuse
// ─────────────────────────────────────────────────────────────────────────────

// A commit without edits hands nothing over: no fade.
TEST(Graph, CommitWithoutChangesDoesNotFade) {
    Graph graph;
    const auto gain = graph.add_node<GainProcessor>(2.0f);
    ASSERT_TRUE(gain);
    ASSERT_TRUE(graph.connect(graph.graph_input(), *gain));
    ASSERT_TRUE(graph.connect(*gain, graph.graph_output()));
    prepare(graph);
    process_blocks(graph, 0.5f, 1);

    EXPECT_FALSE(graph.has_uncommitted_changes());
    ASSERT_TRUE(graph.commit());
    ASSERT_TRUE(graph.commit());
    expect_all(process_blocks(graph, 0.5f, k_fade_blocks), 1.0f);
    auto buffer = make_buffer(0.5f);
    graph.process(buffer);
    expect_all(buffer, 1.0f);
}

// collect_garbage() alone frees a removed node once the audio thread let go of it.
TEST(Graph, CollectGarbageFreesRemovedNodesWithoutAnotherCommit) {
    int destroyed = 0;
    Graph graph;
    const auto node = graph.add_node<LifetimeProcessor>(destroyed);
    ASSERT_TRUE(node);
    ASSERT_TRUE(graph.connect(*node, graph.graph_output()));
    prepare(graph);

    ASSERT_TRUE(graph.remove_node(*node));
    ASSERT_TRUE(graph.commit());
    graph.collect_garbage();
    EXPECT_EQ(destroyed, 0);  // the audio thread still runs the old version

    process_blocks(graph, 0.0f, k_fade_out_blocks);  // switches and hands the old one back
    graph.collect_garbage();
    EXPECT_EQ(destroyed, 1);
}

// One collect_garbage() on the outer graph also frees a nested graph's removed nodes.
TEST(Graph, CollectGarbageReachesNestedGraphs) {
    int destroyed = 0;
    auto owned = std::make_unique<Graph>();
    Graph& inner = *owned;
    const auto node = inner.add_node<LifetimeProcessor>(destroyed);
    ASSERT_TRUE(node);
    ASSERT_TRUE(inner.connect(*node, inner.graph_output()));

    Graph outer;
    const auto nested = outer.add_node(std::move(owned));
    ASSERT_TRUE(nested);
    ASSERT_TRUE(outer.connect(*nested, outer.graph_output()));
    prepare(outer);

    ASSERT_TRUE(inner.remove_node(*node));
    ASSERT_TRUE(inner.commit());
    process_blocks(outer, 0.0f, k_fade_out_blocks);
    EXPECT_EQ(destroyed, 0);
    outer.collect_garbage();
    EXPECT_EQ(destroyed, 1);
}

// A node prepared by the caller for the graph's configuration is not prepared again;
// one prepared for another configuration is.
TEST(Graph, AddPreparedNodePreparesOnlyOnAMismatch) {
    Graph graph;
    prepare(graph);

    auto matching = std::make_unique<PrepareRecorder>();
    matching->prepare(k_sample_rate, k_block, k_channels);
    PrepareRecorder& matching_ref = *matching;
    matching_ref.m_sample_rate = -1.0;  // marks "not prepared by the graph"
    ASSERT_TRUE(graph.add_prepared_node(std::move(matching), k_sample_rate, k_block, k_channels));
    EXPECT_EQ(matching_ref.m_sample_rate, -1.0);

    auto other = std::make_unique<PrepareRecorder>();
    PrepareRecorder& other_ref = *other;
    ASSERT_TRUE(graph.add_prepared_node(std::move(other), 44100.0, k_block, k_channels));
    EXPECT_EQ(other_ref.m_sample_rate, k_sample_rate);
    EXPECT_EQ(other_ref.m_samples_per_block, k_block);
    EXPECT_EQ(other_ref.m_num_channels, k_channels);

    graph.prepare(96000.0, k_block, k_channels);  // prepare() reaches every node again
    EXPECT_EQ(matching_ref.m_sample_rate, 96000.0);
}

// Rewiring back and forth reuses the freed versions' buffers; the output stays right.
TEST(Graph, RepeatedRewiringKeepsTheOutput) {
    Graph graph;
    const auto doubler = graph.add_node<GainProcessor>(2.0f);
    const auto tripler = graph.add_node<GainProcessor>(3.0f);
    const auto constant = graph.add_node<ConstantProcessor>(0.25f);
    ASSERT_TRUE(doubler && tripler && constant);
    ASSERT_TRUE(graph.connect(graph.graph_input(), *doubler));
    ASSERT_TRUE(graph.connect(*doubler, graph.graph_output()));
    average_merges(graph);
    prepare(graph);

    for (int round = 0; round < 6; ++round) {
        const bool tripled = round % 2 == 0;
        ASSERT_TRUE(graph.disconnect(graph.graph_input(), tripled ? *doubler : *tripler));
        ASSERT_TRUE(graph.disconnect(tripled ? *doubler : *tripler, graph.graph_output()));
        ASSERT_TRUE(graph.connect(graph.graph_input(), tripled ? *tripler : *doubler));
        ASSERT_TRUE(graph.connect(tripled ? *tripler : *doubler, graph.graph_output()));
        if (round == 3) { ASSERT_TRUE(graph.connect(*constant, graph.graph_output())); }
        ASSERT_TRUE(graph.commit());
        auto buffer = process_blocks(graph, 0.5f, k_fade_blocks);
        const float gain = tripled ? 1.5f : 1.0f;
        // From round 3 the output averages the gain with the generator.
        expect_all(buffer, round >= 3 ? (gain + 0.25f) * 0.5f : gain);
        graph.collect_garbage();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Latency above the committed maximum, bypass and reset, threads
// ─────────────────────────────────────────────────────────────────────────────

namespace {

/// A latency that can rise, with its maximum, after the graph committed (e.g. a lookahead
/// setting), up to k_capacity.
class AdjustableLatencyProcessor final : public BaseProcessor {
public:
    static constexpr uint32_t k_capacity = 1024;

    explicit AdjustableLatencyProcessor(uint32_t latency)
        : m_latency(latency), m_delay(k_capacity) {}

    void prepare(const double& sample_rate,
                 const size_t& samples_per_block,
                 const size_t& num_channels) override {
        m_delay.prepare(sample_rate, samples_per_block, num_channels);
        m_delay.set_crossfade_samples(0);
        change(m_latency);
    }

    void process(BufferView buffer, uint32_t modulation_offset) TANH_NONBLOCKING_FUNCTION override {
        m_delay.process(buffer, modulation_offset);
    }

    void change(uint32_t latency) {
        m_latency = latency;
        set_max_latency_samples(latency);
        set_latency_samples(latency);
        m_delay.set_delay_samples(latency);
    }

private:
    uint32_t m_latency;
    thl::dsp::utils::DelayProcessor m_delay;
};

/// Outputs the largest input it has seen, until reset(), which it counts.
class PeakHoldProcessor final : public BaseProcessor {
public:
    explicit PeakHoldProcessor(int& resets) : m_resets(resets) {}

    void prepare(const double&, const size_t&, const size_t&) override {}

    void process(BufferView buffer,
                 uint32_t /*modulation_offset*/) TANH_NONBLOCKING_FUNCTION override {
        for (size_t i = 0; i < buffer.get_num_samples(); ++i) {
            for (size_t ch = 0; ch < buffer.get_num_channels(); ++ch) {
                m_peak = std::max(m_peak, buffer.get_read_pointer(ch)[i]);
            }
            for (size_t ch = 0; ch < buffer.get_num_channels(); ++ch) {
                buffer.get_write_pointer(ch)[i] = m_peak;
            }
        }
    }

    void reset() TANH_NONBLOCKING_FUNCTION override {
        m_peak = 0.0f;
        ++m_resets;
    }

private:
    int& m_resets;
    float m_peak = 0.0f;
};

}  // namespace

TEST(Graph, LatencyRaisedAboveTheCommittedMaximumIsAlignedAtItUntilTheNextCommit) {
    Graph graph;
    const auto latent = graph.add_node<AdjustableLatencyProcessor>(10);
    ASSERT_TRUE(latent);
    ASSERT_TRUE(graph.connect(graph.graph_input(), *latent));
    ASSERT_TRUE(graph.connect(*latent, graph.graph_output()));
    ASSERT_TRUE(graph.connect(graph.graph_input(), graph.graph_output()));
    average_merges(graph);
    prepare(graph);
    expect_impulse_response(graph, 10);

    // The node raises its maximum without a commit: the graph keeps compensating at the old one
    // (no assertion, no delay asked beyond what it holds) and says it needs a commit.
    static_cast<AdjustableLatencyProcessor*>(graph.node(*latent))->change(200);
    EXPECT_TRUE(graph.has_uncommitted_changes());
    (void)process_blocks(graph, 0.0f, 8);
    EXPECT_EQ(graph.latency_samples(), 10u);
    EXPECT_LE(graph.latency_samples(), graph.max_latency_samples());

    ASSERT_TRUE(graph.commit());
    EXPECT_EQ(graph.committed_latency_samples(), 200u);
    (void)process_blocks(graph, 0.0f, k_fade_blocks + (200 / k_block) + 1);
    EXPECT_EQ(graph.latency_samples(), 200u);
    expect_impulse_response(graph, 200);
}

TEST(Graph, NestedCommitRaisingTheLatencyIsCompensatedOnceTheOuterGraphCommits) {
    auto owned = std::make_unique<Graph>();
    Graph& inner = *owned;
    ASSERT_TRUE(inner.connect(inner.graph_input(), inner.graph_output()));

    Graph outer;
    const auto nested = outer.add_node(std::move(owned));
    ASSERT_TRUE(nested);
    ASSERT_TRUE(outer.connect(outer.graph_input(), *nested));
    ASSERT_TRUE(outer.connect(*nested, outer.graph_output()));
    ASSERT_TRUE(outer.connect(outer.graph_input(), outer.graph_output()));
    average_merges(outer);
    prepare(outer);
    (void)process_blocks(outer, 0.0f, 4);

    const auto latent = inner.add_node<AdjustableLatencyProcessor>(300);
    ASSERT_TRUE(latent);
    ASSERT_TRUE(inner.disconnect(inner.graph_input(), inner.graph_output()));
    ASSERT_TRUE(inner.connect(inner.graph_input(), *latent));
    ASSERT_TRUE(inner.connect(*latent, inner.graph_output()));
    ASSERT_TRUE(inner.commit());  // the nested graph alone: the outer one compensates at its old
                                  // max
    EXPECT_TRUE(outer.has_uncommitted_changes());
    (void)process_blocks(outer, 0.0f, k_fade_blocks + (300 / k_block) + 1);
    EXPECT_LE(outer.latency_samples(), outer.max_latency_samples());

    ASSERT_TRUE(outer.commit());
    (void)process_blocks(outer, 0.0f, k_fade_blocks + (300 / k_block) + 1);
    EXPECT_EQ(outer.latency_samples(), 300u);
    expect_impulse_response(outer, 300);
}

TEST(Graph, ANodeBackFromBypassIsResetInsteadOfResumingWhereItFroze) {
    int resets = 0;
    Graph graph;
    const auto hold = graph.add_node<PeakHoldProcessor>(resets);
    ASSERT_TRUE(hold);
    ASSERT_TRUE(graph.connect(graph.graph_input(), *hold));
    ASSERT_TRUE(graph.connect(*hold, graph.graph_output()));
    prepare(graph);

    expect_all(process_blocks(graph, 1.0f, 4), 1.0f);  // holds 1
    ASSERT_TRUE(graph.set_wet_mix(*hold, 0.0f));
    (void)process_blocks(graph, 0.0f, k_ramp_blocks + 2);  // bypassed: silence passes
    EXPECT_EQ(resets, 0);
    ASSERT_TRUE(graph.set_wet_mix(*hold, 1.0f));
    // Without the reset it would play the 1 it held before the bypass.
    expect_all(process_blocks(graph, 0.0f, k_ramp_blocks + 2), 0.0f);
    EXPECT_EQ(resets, 1);
}

TEST(Graph, ABypassedNestedGraphComingBackResetsItsNodes) {
    int resets = 0;
    auto owned = std::make_unique<Graph>();
    Graph& inner = *owned;
    const auto hold = inner.add_node<PeakHoldProcessor>(resets);
    ASSERT_TRUE(hold);
    ASSERT_TRUE(inner.connect(inner.graph_input(), *hold));
    ASSERT_TRUE(inner.connect(*hold, inner.graph_output()));

    Graph outer;
    const auto nested = outer.add_node(std::move(owned));
    ASSERT_TRUE(nested);
    ASSERT_TRUE(outer.connect(outer.graph_input(), *nested));
    ASSERT_TRUE(outer.connect(*nested, outer.graph_output()));
    prepare(outer);

    expect_all(process_blocks(outer, 1.0f, 4), 1.0f);
    ASSERT_TRUE(outer.set_wet_mix(*nested, 0.0f));
    (void)process_blocks(outer, 0.0f, k_ramp_blocks + 2);
    ASSERT_TRUE(outer.set_wet_mix(*nested, 1.0f));
    expect_all(process_blocks(outer, 0.0f, k_ramp_blocks + 2), 0.0f);
    EXPECT_EQ(resets, 1);
}

TEST(Graph, EditsAndCommitsWhileTheAudioThreadRuns) {
    // The hand-over under the TSan and RTSan presets: random edits, commits and garbage
    // collection on this thread against process() on another.
    Graph graph;
    const auto nested_id = graph.add_node(std::make_unique<Graph>());
    ASSERT_TRUE(nested_id);
    auto& inner = *static_cast<Graph*>(graph.node(*nested_id));
    ASSERT_TRUE(inner.connect(inner.graph_input(), inner.graph_output()));
    ASSERT_TRUE(graph.connect(graph.graph_input(), *nested_id));
    ASSERT_TRUE(graph.connect(*nested_id, graph.graph_output()));
    prepare(graph);

    std::atomic<bool> stop{false};
    std::atomic<bool> finite{true};
    std::thread audio([&] {
        auto buffer = make_buffer(0.5f);
        while (!stop.load(std::memory_order_acquire)) {
            for (size_t ch = 0; ch < k_channels; ++ch) {
                std::fill_n(buffer.get_write_pointer(ch), k_block, 0.5f);
            }
            graph.process(buffer);
            for (size_t ch = 0; ch < k_channels; ++ch) {
                for (size_t i = 0; i < k_block; ++i) {
                    if (!std::isfinite(buffer.get_read_pointer(ch)[i])) { finite = false; }
                }
            }
        }
    });

    std::mt19937 rng(1);
    std::vector<NodeId> outer_ids;
    std::vector<NodeId> inner_ids;
    for (int edit = 0; edit < 600; ++edit) {
        const auto pick = [&](const std::vector<NodeId>& ids) { return ids[rng() % ids.size()]; };
        switch (rng() % 7) {
            case 0: {
                const auto id =
                    graph.add_node<LatencyProcessor>(static_cast<uint32_t>(rng() % 300));
                outer_ids.push_back(*id);
                graph.connect(graph.graph_input(), *id);
                graph.connect(*id, graph.graph_output());
                break;
            }
            case 1:
                if (!outer_ids.empty()) {
                    const size_t i = rng() % outer_ids.size();
                    graph.remove_node(outer_ids[i]);
                    outer_ids.erase(outer_ids.begin() + static_cast<std::ptrdiff_t>(i));
                }
                break;
            case 2:
                if (!outer_ids.empty()) { graph.connect(pick(outer_ids), pick(outer_ids)); }
                break;
            case 3:
                if (!outer_ids.empty()) {
                    graph.set_wet_mix(pick(outer_ids), static_cast<float>(rng() % 3) / 2.0f);
                }
                break;
            case 4: {
                const auto id =
                    inner.add_node<LatencyProcessor>(static_cast<uint32_t>(rng() % 100));
                inner_ids.push_back(*id);
                inner.connect(inner.graph_input(), *id);
                inner.connect(*id, inner.graph_output());
                break;
            }
            case 5:
                if (!inner_ids.empty()) {
                    inner.remove_node(inner_ids.back());
                    inner_ids.pop_back();
                }
                break;
            default: break;
        }
        ASSERT_TRUE(graph.commit());
        if (rng() % 4 == 0) { graph.collect_garbage(); }
        if (rng() % 8 == 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(rng() % 1000));
        }
    }
    stop.store(true, std::memory_order_release);
    audio.join();
    EXPECT_TRUE(finite.load());
    EXPECT_LE(graph.latency_samples(), graph.max_latency_samples());
}

TEST(Graph, ReportsAndHasTheLeastLatencyItsNodesAllowNotTheirMaximum) {
    Graph graph;
    const auto latent = graph.add_node<LatencyProcessor>(10, 100);  // e.g. lookahead 10, at most
                                                                    // 100
    ASSERT_TRUE(latent);
    ASSERT_TRUE(graph.connect(graph.graph_input(), *latent));
    ASSERT_TRUE(graph.connect(*latent, graph.graph_output()));
    ASSERT_TRUE(graph.connect(graph.graph_input(), graph.graph_output()));
    average_merges(graph);
    prepare(graph);
    EXPECT_EQ(graph.committed_latency_samples(), 10u);  // what the host is told
    EXPECT_EQ(graph.max_latency_samples(), 100u);       // only what the delays hold
    expect_impulse_response(graph, 10);                 // no padding to the maximum
    EXPECT_EQ(graph.latency_samples(), 10u);

    // Within its maximum the node changes without a commit; the graph follows at the next block.
    static_cast<LatencyProcessor*>(graph.node(*latent))->set_latency(50);
    EXPECT_FALSE(graph.has_uncommitted_changes());
    (void)process_blocks(graph, 0.0f, 1);
    EXPECT_EQ(graph.latency_samples(), 50u);
    (void)process_blocks(graph, 0.0f, 4);
    expect_impulse_response(graph, 50);
}

// ─────────────────────────────────────────────────────────────────────────────
// Mix laws
// ─────────────────────────────────────────────────────────────────────────────

namespace {

struct Gains {
    float m_dry;
    float m_wet;
};

/// The gains a node with @p law at wet mix @p m applies, measured: a processor that outputs @p wet
/// whatever its input, fed @p dry.
float measure(Graph::MixLaw law, float m, float dry, float wet) {
    Graph graph;
    const auto node = graph.add_node<ConstantProcessor>(wet);
    EXPECT_TRUE(node);
    EXPECT_TRUE(graph.connect(graph.graph_input(), *node));
    EXPECT_TRUE(graph.connect(*node, graph.graph_output()));
    EXPECT_TRUE(graph.set_mix_law(*node, law));
    EXPECT_TRUE(graph.set_wet_mix(*node, m));
    prepare(graph);  // starts at the set mix, no ramp
    return process_blocks(graph, dry, 1).get_read_pointer(0)[0];
}

Gains measure_gains(Graph::MixLaw law, float m) {
    return {.m_dry = measure(law, m, 1.0f, 0.0f), .m_wet = measure(law, m, 0.0f, 1.0f)};
}

Gains expected_gains(Graph::MixLaw law, double m) {
    const double q = 0.5 * std::numbers::pi * m;
    switch (law) {
        case Graph::MixLaw::Linear: return {static_cast<float>(1.0 - m), static_cast<float>(m)};
        case Graph::MixLaw::ConstantPower:
            return {static_cast<float>(std::cos(q)), static_cast<float>(std::sin(q))};
        case Graph::MixLaw::Compromise:
            return {static_cast<float>(std::sqrt((1.0 - m) * std::cos(q))),
                    static_cast<float>(std::sqrt(m * std::sin(q)))};
        case Graph::MixLaw::Parallel: return {1.0f, static_cast<float>(m)};
        case Graph::MixLaw::DryHeld:
            return {static_cast<float>(std::min(1.0, 2.0 * (1.0 - m))),
                    static_cast<float>(std::min(1.0, 2.0 * m))};
    }
    return {};
}

float db(float gain) {
    return 20.0f * std::log10(gain);
}

}  // namespace

TEST(Graph, EveryMixLawSetsTheDryAndWetGainsItDescribes) {
    for (const auto law : {Graph::MixLaw::Linear,
                           Graph::MixLaw::ConstantPower,
                           Graph::MixLaw::Compromise,
                           Graph::MixLaw::Parallel,
                           Graph::MixLaw::DryHeld}) {
        for (const float m : {0.0f, 0.1f, 0.25f, 0.5f, 0.75f, 0.9f, 1.0f}) {
            const Gains got = measure_gains(law, m);
            const Gains want = expected_gains(law, m);
            EXPECT_NEAR(got.m_dry, want.m_dry, 1e-5f) << static_cast<int>(law) << " at " << m;
            EXPECT_NEAR(got.m_wet, want.m_wet, 1e-5f) << static_cast<int>(law) << " at " << m;
        }
    }
}

TEST(Graph, MixLawsMeetTheirLevelsAtHalf) {
    const Gains linear = measure_gains(Graph::MixLaw::Linear, 0.5f);
    EXPECT_NEAR(db(linear.m_dry), -6.02f, 0.01f);  // amplitudes add up to 1
    EXPECT_FLOAT_EQ(linear.m_dry + linear.m_wet, 1.0f);
    const Gains power = measure_gains(Graph::MixLaw::ConstantPower, 0.5f);
    EXPECT_NEAR(db(power.m_dry), -3.01f, 0.01f);  // powers add up to 1
    EXPECT_NEAR((power.m_dry * power.m_dry) + (power.m_wet * power.m_wet), 1.0f, 1e-5f);
    EXPECT_NEAR(db(measure_gains(Graph::MixLaw::Compromise, 0.5f).m_wet), -4.52f, 0.01f);
    const Gains parallel = measure_gains(Graph::MixLaw::Parallel, 1.0f);
    EXPECT_EQ(parallel.m_dry, 1.0f);  // fully wet still keeps the dry signal
    EXPECT_EQ(parallel.m_wet, 1.0f);
    const Gains held = measure_gains(Graph::MixLaw::DryHeld, 0.5f);
    EXPECT_EQ(held.m_dry, 1.0f);
    EXPECT_EQ(held.m_wet, 1.0f);
}

TEST(Graph, ChangingTheMixLawGlides) {
    Graph graph;
    const auto node = graph.add_node<ConstantProcessor>(1.0f);
    ASSERT_TRUE(node);
    ASSERT_TRUE(graph.connect(graph.graph_input(), *node));
    ASSERT_TRUE(graph.connect(*node, graph.graph_output()));
    prepare(graph);
    expect_all(process_blocks(graph, 1.0f, 1), 1.0f);  // fully wet, linear: the constant

    ASSERT_TRUE(graph.set_mix_law(*node, Graph::MixLaw::Parallel));  // adds the dry 1 on top
    EXPECT_FALSE(graph.has_uncommitted_changes());
    const auto first = process_blocks(graph, 1.0f, 1);
    const float* samples = first.get_read_pointer(0);
    EXPECT_GT(samples[0], 1.0f);
    EXPECT_LT(samples[0], 1.01f);  // starts gently
    for (size_t i = 1; i < k_block; ++i) { EXPECT_GE(samples[i], samples[i - 1]); }
    expect_all(process_blocks(graph, 1.0f, k_ramp_blocks), 2.0f);
}

TEST(Graph, MixLawRejectsEndpointsAndUnknownNodes) {
    Graph graph;
    const auto node = graph.add_node<GainProcessor>(1.0f);
    ASSERT_TRUE(node);
    EXPECT_EQ(graph.mix_law(*node), Graph::MixLaw::Linear);
    EXPECT_TRUE(graph.set_mix_law(*node, Graph::MixLaw::ConstantPower));
    EXPECT_EQ(graph.mix_law(*node), Graph::MixLaw::ConstantPower);
    EXPECT_FALSE(graph.set_mix_law(graph.graph_input(), Graph::MixLaw::Parallel));
    EXPECT_FALSE(graph.set_mix_law(graph.graph_output(), Graph::MixLaw::Parallel));
    EXPECT_FALSE(graph.set_mix_law(NodeId{999}, Graph::MixLaw::Parallel));
    EXPECT_FALSE(graph.mix_law(graph.graph_output()).has_value());
}

TEST(Graph, ANodeWhoseMaximumChangedCountsAtItUntilItRuns) {
    // A node wrapping something that already knows its new latency (e.g. a graph of its own that
    // just committed), while its own latency_samples() still tells the old one until it runs.
    class Wrapper final : public BaseProcessor {
    public:
        void prepare(const double&, const size_t&, const size_t&) override {}
        void process(BufferView, uint32_t) TANH_NONBLOCKING_FUNCTION override {
            set_latency_samples(m_inner);
        }
        void announce(uint32_t inner) {  // control thread: the new maximum, not yet the latency
            m_inner = inner;
            set_max_latency_samples(inner);
        }

    private:
        uint32_t m_inner = 0;
    };

    Graph graph;
    const auto wrapper = graph.add_node<Wrapper>();
    ASSERT_TRUE(wrapper);
    ASSERT_TRUE(graph.connect(graph.graph_input(), *wrapper));
    ASSERT_TRUE(graph.connect(*wrapper, graph.graph_output()));
    prepare(graph);
    EXPECT_EQ(graph.committed_latency_samples(), 0u);

    static_cast<Wrapper*>(graph.node(*wrapper))->announce(24);
    ASSERT_TRUE(graph.commit());
    EXPECT_EQ(graph.committed_latency_samples(), 24u);  // known before the audio thread runs it
    (void)process_blocks(graph, 0.0f, k_fade_blocks + 2);
    EXPECT_EQ(graph.latency_samples(), 24u);
}
