#include "tanh/graph/Graph.h"
#include <tanh/core/Buffer.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>
#include "tanh/graph/Node.h"

namespace thl::graph {

namespace {

/// The graph's forced output: one input port, no outputs. process() is a
/// no-op; Graph::process() copies the buffer feeding its input to the
/// external output.
class OutputNode final : public Node {
public:
    explicit OutputNode(size_t num_channels)
        : m_layout{.inputs = {num_channels}, .outputs = {}} {}
    PortLayout ports() const override { return m_layout; }
    void process(const ProcessContext&) TANH_NONBLOCKING_FUNCTION override {}

private:
    PortLayout m_layout;
};

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// The compiled snapshot. Built on the control thread, read by the audio thread.
// The audio thread writes into GraphBuffers every block, so it sits behind a
// pointer: RCU readers only get const access to the snapshot itself.
// ─────────────────────────────────────────────────────────────────────────────

struct Graph::GraphBuffers {
    std::vector<thl::core::Buffer<float>> buffers;        // zeros, then one per output port
    std::vector<thl::core::ConstBufferView> input_views;  // reused by every step
    std::vector<thl::core::BufferView> output_views;      // reused by every step
};

struct Graph::ProcessingStep {
    Node* node = nullptr;
    PortLayout ports;
    std::vector<BufferIndex> input_buffers;   // per input port; k_zeros_buffer if unconnected
    std::vector<BufferIndex> output_buffers;  // per output port
};

struct Graph::CompiledGraph {
    std::vector<std::shared_ptr<Node>> nodes;    // keeps nodes alive while this snapshot is in use
    std::vector<ProcessingStep> steps;           // in processing order
    std::unique_ptr<GraphBuffers> buffers;
    BufferIndex output_buffer = k_zeros_buffer;  // what feeds the output node
    size_t max_block_size = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

Graph::Graph(size_t num_output_channels) : m_compiled_graph(CompiledGraph{}) {
    m_nodes.emplace(k_output_id, std::make_shared<OutputNode>(num_output_channels));
}

Graph::~Graph() = default;

// ─────────────────────────────────────────────────────────────────────────────
// Editing
// ─────────────────────────────────────────────────────────────────────────────

NodeId Graph::add_node_impl(std::shared_ptr<Node> node) {
    if (m_spec.max_block_size > 0) {
        node->prepare(m_spec);
    }
    const NodeId id{m_next_id++};
    m_nodes.emplace(id, std::move(node));
    return id;
}

bool Graph::remove_node(NodeId id) {
    if (id == k_output_id) { return false; }
    if (m_nodes.erase(id) == 0) { return false; }

    std::erase_if(m_connections, [id](const auto& entry) {
        const auto& [to, from] = entry;
        return to.node == id || from.node == id;
    });
    return true;
}

bool Graph::path_exists(NodeId from, NodeId to) const {
    std::vector<NodeId> stack{from};
    std::vector<NodeId> visited;
    while (!stack.empty()) {
        const NodeId n = stack.back();
        stack.pop_back();
        if (n == to) { return true; }
        if (std::ranges::find(visited, n) != visited.end()) { continue; }
        visited.push_back(n);
        for (const auto& [to, from] : m_connections) {
            if (from.node == n) { stack.push_back(to.node); }
        }
    }
    return false;
}

bool Graph::connect(PortRef from, PortRef to) {
    if (!exists(from.node) || !exists(to.node) || from.node == to.node) { return false; }

    const PortLayout from_ports = m_nodes.at(from.node)->ports();
    const PortLayout to_ports = m_nodes.at(to.node)->ports();
    if (from.port >= from_ports.outputs.size() || to.port >= to_ports.inputs.size()) {
        return false;
    }
    if (from_ports.outputs[from.port] != to_ports.inputs[to.port]) { return false; }

    if (m_connections.contains(to)) { return false; }  // an input has exactly one source
    if (path_exists(to.node, from.node)) { return false; }

    m_connections.emplace(to, from);
    return true;
}

bool Graph::disconnect(PortRef to) { return m_connections.erase(to) > 0; }

// ─────────────────────────────────────────────────────────────────────────────
// Prepare / compile / publish
// ─────────────────────────────────────────────────────────────────────────────

void Graph::prepare(const ProcessSpec& spec) {
    m_spec = spec;
    for (auto& [id, node] : m_nodes) {
        node->prepare(spec);
    }
    commit();
}

std::optional<std::vector<NodeId>> Graph::sort_graph_dfs() const {
    enum class VisitState : uint8_t { InProgress, Done };
    struct Frame {
        NodeId node;
        size_t num_inputs;
        uint32_t next_port = 0;
    };

    std::map<NodeId, VisitState> state;
    std::vector<NodeId> order;
    std::vector<Frame> stack;

    const auto push = [&](NodeId id) {
        state[id] = VisitState::InProgress;
        stack.push_back({.node = id, .num_inputs = m_nodes.at(id)->ports().inputs.size()});
    };
    push(k_output_id);

    while (!stack.empty()) {
        Frame& frame = stack.back();

        if (frame.next_port == frame.num_inputs) {  // every source of this node is in `order`
            state[frame.node] = VisitState::Done;
            order.push_back(frame.node);
            stack.pop_back();
            continue;
        }

        const auto source = m_connections.find(PortRef{.node = frame.node, .port = frame.next_port++});
        if (source == m_connections.end()) { continue; }  // unconnected input

        const NodeId source_node = source->second.node;
        const auto it = state.find(source_node);
        if (it == state.end()) {
            push(source_node);  // may invalidate `frame`; it is not used again this iteration
        } else if (it->second == VisitState::InProgress) {
            return std::nullopt;  // back on our own path: cycle
        }
    }
    return order;
}

std::optional<Graph::CompiledGraph> Graph::compile() const {
    const auto order = sort_graph_dfs();
    if (!order) { return std::nullopt; }

    CompiledGraph compiled;
    compiled.max_block_size = m_spec.max_block_size;
    compiled.buffers = std::make_unique<GraphBuffers>();
    auto& buffers = compiled.buffers->buffers;

    buffers.emplace_back();  // k_zeros_buffer; sized after the loop, once the widest input is known
    size_t max_input_channels = 0;
    size_t max_inputs = 0;
    size_t max_outputs = 0;

    // `order` has every node after its sources, so a source's buffer always
    // exists by the time an input looks it up.
    std::map<PortRef, BufferIndex> output_buffers;  // output port -> its buffer
    for (NodeId id : *order) {
        const std::shared_ptr<Node>& node = m_nodes.at(id);
        const PortLayout layout = node->ports();
        ProcessingStep step{.node = node.get(), .ports = layout};

        for (uint32_t port = 0; port < layout.inputs.size(); ++port) {
            const auto source = m_connections.find(PortRef{.node = id, .port = port});
            step.input_buffers.push_back(source == m_connections.end()
                                             ? k_zeros_buffer
                                             : output_buffers.at(source->second));
            max_input_channels = std::max(max_input_channels, layout.inputs[port]);
        }
        for (uint32_t port = 0; port < layout.outputs.size(); ++port) {
            const auto index = static_cast<BufferIndex>(buffers.size());
            buffers.emplace_back(layout.outputs[port], m_spec.max_block_size);
            output_buffers.emplace(PortRef{.node = id, .port = port}, index);
            step.output_buffers.push_back(index);
        }
        max_inputs = std::max(max_inputs, layout.inputs.size());
        max_outputs = std::max(max_outputs, layout.outputs.size());

        compiled.nodes.push_back(node);
        compiled.steps.push_back(std::move(step));
    }

    buffers[k_zeros_buffer] = thl::core::Buffer<float>(max_input_channels, m_spec.max_block_size);
    compiled.buffers->input_views.resize(max_inputs);
    compiled.buffers->output_views.resize(max_outputs);

    const auto source = m_connections.find(PortRef{.node = k_output_id, .port = 0});
    if (source != m_connections.end()) { compiled.output_buffer = output_buffers.at(source->second); }

    return compiled;
}

bool Graph::commit() {
    auto compiled = compile();
    if (!compiled) { return false; }

    // The previous snapshot is retired and freed later, on this thread, once
    // the audio thread has left it.
    m_compiled_graph.replace([&](CompiledGraph& g) { g = std::move(*compiled); });
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Processing
// ─────────────────────────────────────────────────────────────────────────────

void Graph::register_audio_thread() const { m_compiled_graph.register_reader_thread(); }

void Graph::process(thl::core::BufferView output) {
    const auto scope = m_compiled_graph.read_scope();
    const CompiledGraph& g = scope.data();
    const size_t n = output.get_num_samples();

    // Silence until prepare() is called, or if the block is longer than prepare() allowed.
    if (g.max_block_size == 0 || n > g.max_block_size) {
        for (size_t ch = 0; ch < output.get_num_channels(); ++ch) {
            std::fill_n(output.get_write_pointer(ch), n, 0.0f);
        }
        return;
    }
    GraphBuffers& b = *g.buffers;

    for (const ProcessingStep& step : g.steps) {
        for (size_t p = 0; p < step.input_buffers.size(); ++p) {
            const auto& buffer = b.buffers[step.input_buffers[p]];
            b.input_views[p] = thl::core::ConstBufferView(buffer.get_array_of_read_pointers(),
                                                          step.ports.inputs[p], n);
        }
        for (size_t p = 0; p < step.output_buffers.size(); ++p) {
            auto& buffer = b.buffers[step.output_buffers[p]];
            b.output_views[p] = thl::core::BufferView(buffer.get_array_of_write_pointers(),
                                                      step.ports.outputs[p], n);
        }
        step.node->process({.inputs = std::span(b.input_views).first(step.input_buffers.size()),
                            .outputs = std::span(b.output_views).first(step.output_buffers.size()),
                            .num_frames = n});
    }

    const auto& out_buf = b.buffers[g.output_buffer];
    for (size_t ch = 0; ch < output.get_num_channels(); ++ch) {
        float* d = output.get_write_pointer(ch);
        if (ch < out_buf.get_num_channels()) {
            std::copy_n(out_buf.get_read_pointer(ch), n, d);
        } else {
            std::fill_n(d, n, 0.0f);
        }
    }
}

}  // namespace thl::graph
