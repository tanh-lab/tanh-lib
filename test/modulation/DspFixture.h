// Shared helpers for DSP processor click/regression tests. Each per-processor
// test file defines its own wrapper (subclass of the processor + SmartHandle
// wiring) and its own LFO setup, then uses these utilities to drive the
// processor through the modulation matrix and assert on the output.
//
// Every driver takes a Host: the processor runs directly, or as one node of a
// multi-node graph, or inside a nested graph whose block the host runs in parts
// (the modulation offset). The graph hosts are identities around the processor,
// so their output must match the direct one exactly.
//
// Universal properties asserted by the standard tests:
//   1. Silence in → silence out under static (non-default) parameter values
//      [for effects only; sources/synths have their own variant].
//   2. No per-sample clicks under knob-rate parameter modulation, silent input.
//   3. No per-sample clicks under knob-rate parameter modulation, signal input.
//   4. Finite output (no NaN/Inf) — folded into the above via expect_all_finite.

#pragma once

#include <gtest/gtest.h>
#include <tanh/core/BufferView.h>
#include <tanh/dsp/BaseProcessor.h>
#include <tanh/modulation/ModulationMatrix.h>
#if defined(TANH_GRAPH_ENABLED)
#include <tanh/graph/Graph.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "TestHelpers.h"

namespace dsp_fixture {

inline float max_abs(const std::vector<float>& v) {
    float m = 0.0f;
    for (float s : v) { m = std::max(m, std::abs(s)); }
    return m;
}

inline float max_sample_step(const std::vector<float>& v) {
    float m = 0.0f;
    for (size_t i = 1; i < v.size(); ++i) { m = std::max(m, std::abs(v[i] - v[i - 1])); }
    return m;
}

inline void expect_all_finite(const std::vector<float>& v) {
    for (float s : v) { ASSERT_TRUE(std::isfinite(s)) << "non-finite sample"; }
}

// Where a driver runs the processor.
enum class Host : uint8_t {
    Direct,       ///< process_modulated() on the processor
    Graph,        ///< a node of a graph with unity nodes around it and a silent parallel branch
    NestedSplit,  ///< that graph nested in another one, every block run in three parts
};

inline std::vector<Host> hosts() {
#if defined(TANH_GRAPH_ENABLED)
    return {Host::Direct, Host::Graph, Host::NestedSplit};
#else
    return {Host::Direct};
#endif
}

inline std::string host_name(const ::testing::TestParamInfo<Host>& info) {
    switch (info.param) {
        case Host::Direct: return "Direct";
        case Host::Graph: return "Graph";
        case Host::NestedSplit: return "NestedSplit";
    }
    return "Unknown";
}

#if defined(TANH_GRAPH_ENABLED)
/// The test's processor as a graph node. The test owns and prepares it.
class ProcessorRef final : public thl::dsp::BaseProcessor {
public:
    explicit ProcessorRef(thl::dsp::BaseProcessor& processor) : m_processor(processor) {}
    void prepare(const double&, const size_t&, const size_t&) override {}
    void process(thl::core::BufferView buffer,
                 uint32_t modulation_offset) TANH_NONBLOCKING_FUNCTION override {
        m_processor.process_modulated(buffer, modulation_offset);
    }

private:
    thl::dsp::BaseProcessor& m_processor;
};

/// Leaves the buffer as it is.
class Unity final : public thl::dsp::BaseProcessor {
public:
    void prepare(const double&, const size_t&, const size_t&) override {}
    void process(thl::core::BufferView, uint32_t) TANH_NONBLOCKING_FUNCTION override {}
};

/// Writes silence: a parallel branch that adds nothing at a summing merge.
class Silence final : public thl::dsp::BaseProcessor {
public:
    void prepare(const double&, const size_t&, const size_t&) override {}
    void process(thl::core::BufferView buffer, uint32_t) TANH_NONBLOCKING_FUNCTION override {
        for (size_t ch = 0; ch < buffer.get_num_channels(); ++ch) {
            std::fill_n(buffer.get_write_pointer(ch), buffer.get_num_samples(), 0.0f);
        }
    }
};

/// input -> unity -> processor -> unity -> output, plus input -> silence -> output (summed).
inline std::unique_ptr<thl::graph::Graph> make_graph(thl::dsp::BaseProcessor& processor) {
    using thl::graph::Graph;
    auto graph = std::make_unique<Graph>();
    const auto pre = graph->add_node<Unity>();
    const auto node = graph->add_node<ProcessorRef>(processor);
    const auto post = graph->add_node<Unity>();
    const auto silence = graph->add_node<Silence>();
    EXPECT_TRUE(pre && node && post && silence);
    EXPECT_TRUE(graph->connect(graph->graph_input(), *pre));
    EXPECT_TRUE(graph->connect(*pre, *node));
    EXPECT_TRUE(graph->connect(*node, *post));
    EXPECT_TRUE(graph->connect(*post, graph->graph_output()));
    EXPECT_TRUE(graph->connect(graph->graph_input(), *silence));
    EXPECT_TRUE(graph->connect(*silence, graph->graph_output()));
    EXPECT_TRUE(graph->set_input_mix(graph->graph_output(), Graph::InputMix::Sum));
    return graph;
}
#endif

/// Runs a prepared processor in @p host, one block per process() call.
class Hosted {
public:
    Hosted(thl::dsp::BaseProcessor& processor, Host host, size_t num_channels)
        : m_processor(processor), m_host(host) {
#if defined(TANH_GRAPH_ENABLED)
        if (host == Host::Graph) { m_graph = make_graph(processor); }
        if (host == Host::NestedSplit) {
            m_graph = std::make_unique<thl::graph::Graph>();
            const auto inner = m_graph->add_node(make_graph(processor));
            const auto post = m_graph->add_node<Unity>();
            EXPECT_TRUE(inner && post);
            EXPECT_TRUE(m_graph->connect(m_graph->graph_input(), *inner));
            EXPECT_TRUE(m_graph->connect(*inner, *post));
            EXPECT_TRUE(m_graph->connect(*post, m_graph->graph_output()));
        }
        if (m_graph) { m_graph->prepare(k_sample_rate, k_block_size, num_channels); }
#else
        (void)num_channels;
#endif
    }

    void process(const thl::core::BufferView& view) {
#if defined(TANH_GRAPH_ENABLED)
        if (m_host == Host::Graph) {
            m_graph->process(view, 0);
            return;
        }
        if (m_host == Host::NestedSplit) {
            // Three parts that cut across the processors' change points.
            const size_t n = view.get_num_samples();
            const size_t a = std::min<size_t>(n, 100);
            const size_t b = std::min<size_t>(n, 333);
            m_graph->process(view.sub_block(0, a), 0);
            if (b > a) { m_graph->process(view.sub_block(a, b - a), static_cast<uint32_t>(a)); }
            if (n > b) { m_graph->process(view.sub_block(b, n - b), static_cast<uint32_t>(b)); }
            return;
        }
#endif
        m_processor.process_modulated(view);
    }

private:
    thl::dsp::BaseProcessor& m_processor;
    Host m_host;
#if defined(TANH_GRAPH_ENABLED)
    std::unique_ptr<thl::graph::Graph> m_graph;
#endif
};

// Mono driver. Runs N blocks of input through a processor while advancing
// the modulation matrix once per block. Returns the (mutated) output flat.
template <typename Processor>
std::vector<float> run_blocks_mono(thl::modulation::ModulationMatrix& matrix,
                                   Processor& proc,
                                   const std::vector<float>& input,
                                   Host host = Host::Direct) {
    Hosted hosted(proc, host, 1);
    std::vector<float> output;
    output.reserve(input.size());
    const size_t num_blocks = input.size() / k_block_size;
    std::vector<float> block(k_block_size);
    for (size_t b = 0; b < num_blocks; ++b) {
        std::copy(input.begin() + static_cast<ptrdiff_t>(b * k_block_size),
                  input.begin() + static_cast<ptrdiff_t>((b + 1) * k_block_size),
                  block.begin());
        matrix.process(k_block_size);
        thl::core::BufferView view(block.data(), k_block_size);
        hosted.process(view);
        output.insert(output.end(), block.begin(), block.end());
    }
    return output;
}

// Stereo driver. Runs N blocks of mono `input` duplicated onto two channels
// through a 2-channel processor. Returns each channel's output flat.
template <typename Processor>
std::pair<std::vector<float>, std::vector<float>> run_blocks_stereo(
    thl::modulation::ModulationMatrix& matrix,
    Processor& proc,
    const std::vector<float>& input_mono,
    Host host = Host::Direct) {
    Hosted hosted(proc, host, 2);
    std::vector<float> out_l, out_r;
    out_l.reserve(input_mono.size());
    out_r.reserve(input_mono.size());

    const size_t num_blocks = input_mono.size() / k_block_size;
    std::vector<float> l(k_block_size), r(k_block_size);
    std::array<float*, 2> channels{l.data(), r.data()};

    for (size_t b = 0; b < num_blocks; ++b) {
        for (size_t i = 0; i < k_block_size; ++i) {
            const float sample = input_mono[b * k_block_size + i];
            l[i] = sample;
            r[i] = sample;
        }
        matrix.process(k_block_size);
        thl::core::BufferView view(channels.data(), 2, k_block_size);
        hosted.process(view);
        out_l.insert(out_l.end(), l.begin(), l.end());
        out_r.insert(out_r.end(), r.begin(), r.end());
    }
    return {std::move(out_l), std::move(out_r)};
}

// 220 Hz sine, 0.5 amplitude — the standard signal input for click tests.
inline std::vector<float> make_test_sine(size_t num_blocks,
                                         float frequency_hz = 220.0f,
                                         float amplitude = 0.5f) {
    constexpr float k_two_pi = 6.28318530718f;
    std::vector<float> v(num_blocks * k_block_size);
    for (size_t i = 0; i < v.size(); ++i) {
        v[i] = amplitude * std::sin(k_two_pi * frequency_hz * static_cast<float>(i) /
                                    static_cast<float>(k_sample_rate));
    }
    return v;
}

// "Knob-like" LFO frequencies: slow enough that the 5 ms smoothing window
// fully resolves between transitions, but each parameter steps independently.
// These are intentionally co-prime-ish so the four+ LFOs don't align.
inline constexpr float k_knob_lfo_freqs[] = {5.0f,
                                             7.0f,
                                             11.0f,
                                             13.0f,
                                             17.0f,
                                             19.0f,
                                             23.0f,
                                             29.0f,
                                             31.0f,
                                             37.0f,
                                             41.0f,
                                             43.0f,
                                             47.0f,
                                             53.0f,
                                             59.0f};

}  // namespace dsp_fixture
