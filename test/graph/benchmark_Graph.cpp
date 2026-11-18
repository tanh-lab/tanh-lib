// Control-thread cost of a commit (rewire and no-op) and the audio cost of a
// block, for graphs of 10 and 50 gain nodes in a chain.

#include <benchmark/benchmark.h>
#include <tanh/core/Buffer.h>
#include <tanh/core/BufferView.h>
#include <tanh/dsp/BaseProcessor.h>
#include <tanh/graph/Graph.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

using thl::graph::Graph;
using thl::graph::NodeId;

namespace {

constexpr double k_sample_rate = 48000.0;
constexpr size_t k_block = 256;
constexpr size_t k_channels = 2;

class Gain final : public thl::dsp::BaseProcessor {
public:
    void prepare(const double&, const size_t&, const size_t&) override {}
    void process(thl::core::BufferView buffer, uint32_t) TANH_NONBLOCKING_FUNCTION override {
        for (size_t ch = 0; ch < buffer.get_num_channels(); ++ch) {
            float* samples = buffer.get_write_pointer(ch);
            for (size_t i = 0; i < buffer.get_num_samples(); ++i) { samples[i] *= 0.999f; }
        }
    }
};

/// input -> n gain nodes in a chain -> output, prepared.
struct Chain {
    explicit Chain(size_t n) {
        NodeId previous = graph.graph_input();
        for (size_t i = 0; i < n; ++i) {
            const auto id = *graph.add_node<Gain>();
            graph.connect(previous, id);
            nodes.push_back(id);
            previous = id;
        }
        graph.connect(previous, graph.graph_output());
        graph.prepare(k_sample_rate, k_block, k_channels);
    }

    Graph graph;
    std::vector<NodeId> nodes;
};

}  // namespace

// A rewire per commit: a skip edge from the input to the middle node, on and off.
static void bm_graph_commit_rewire(benchmark::State& state) {
    Chain chain(static_cast<size_t>(state.range(0)));
    const NodeId middle = chain.nodes[chain.nodes.size() / 2];
    bool on = false;
    for ([[maybe_unused]] auto _ : state) {
        on = !on;
        if (on) {
            chain.graph.connect(chain.graph.graph_input(), middle);
        } else {
            chain.graph.disconnect(chain.graph.graph_input(), middle);
        }
        benchmark::DoNotOptimize(chain.graph.commit());
    }
}
BENCHMARK(bm_graph_commit_rewire)->Arg(10)->Arg(50);

// A commit without edits.
static void bm_graph_commit_noop(benchmark::State& state) {
    Chain chain(static_cast<size_t>(state.range(0)));
    for ([[maybe_unused]] auto _ : state) { benchmark::DoNotOptimize(chain.graph.commit()); }
}
BENCHMARK(bm_graph_commit_noop)->Arg(10)->Arg(50);

// One audio block.
static void bm_graph_process(benchmark::State& state) {
    Chain chain(static_cast<size_t>(state.range(0)));
    thl::core::Buffer<float> buffer(k_channels, k_block);
    const thl::core::BufferView view(buffer.get_array_of_write_pointers(), k_channels, k_block);
    for ([[maybe_unused]] auto _ : state) {
        chain.graph.process(view, 0);
        benchmark::DoNotOptimize(buffer.get_read_pointer(0));
    }
}
BENCHMARK(bm_graph_process)->Arg(10)->Arg(50);

BENCHMARK_MAIN();
