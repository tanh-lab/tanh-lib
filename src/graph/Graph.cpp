#include <tanh/core/Buffer.h>
#include <tanh/core/BufferView.h>
#include <tanh/graph/Graph.h>
#include <tanh/graph/Node.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <utility>
#include <vector>

namespace thl::graph {

namespace {

/// The graph's forced output: one input port, no outputs. process() is a
/// no-op; Graph::process() copies the buffer feeding its input to the
/// external output.
class OutputNode final : public Node {
public:
    explicit OutputNode(size_t num_channels)
        : m_layout{.m_inputs = {num_channels}, .m_outputs = {}} {}
    PortLayout ports() const override { return m_layout; }

private:
    void process(const ProcessContext&) TANH_NONBLOCKING_FUNCTION override {}

    PortLayout m_layout;
};

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// The compiled snapshot. Built on the control thread, read by the audio thread.
// The audio thread writes into GraphBuffers every block, so it sits behind a
// pointer: RCU readers only get const access to the snapshot itself.
// ─────────────────────────────────────────────────────────────────────────────

struct Graph::GraphBuffers {
    std::vector<thl::core::Buffer<float>> m_buffers;        // zeros, then one per output port
    std::vector<thl::core::ConstBufferView> m_input_views;  // reused by every step
    std::vector<thl::core::BufferView> m_output_views;      // reused by every step
};

struct Graph::ProcessingStep {
    Node* m_node = nullptr;
    PortLayout m_ports;
    std::vector<BufferIndex> m_input_buffers;   // per input port; k_zeros_buffer if unconnected
    std::vector<BufferIndex> m_output_buffers;  // per output port
};

struct Graph::CompiledGraph {
    std::vector<std::shared_ptr<Node>> m_nodes;  // keeps nodes alive while this snapshot is in use
    std::vector<ProcessingStep> m_steps;         // in processing order
    std::unique_ptr<GraphBuffers> m_buffers;
    BufferIndex m_output_buffer = k_zeros_buffer;  // what feeds the output node
    size_t m_max_block_size = 0;
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

Node* Graph::node(NodeId id) {
    const auto it = m_nodes.find(id);
    return it == m_nodes.end() ? nullptr : it->second.get();
}

const Node* Graph::node(NodeId id) const {
    const auto it = m_nodes.find(id);
    return it == m_nodes.end() ? nullptr : it->second.get();
}

std::vector<NodeId> Graph::nodes() const {
    std::vector<NodeId> ids;
    ids.reserve(m_nodes.size());
    for (const auto& [id, node] : m_nodes) { ids.push_back(id); }
    return ids;
}

std::vector<Connection> Graph::connections() const {
    std::vector<Connection> result;
    result.reserve(m_connections.size());
    for (const auto& [to, from] : m_connections) { result.push_back({.m_from = from, .m_to = to}); }
    return result;
}

std::optional<PortRef> Graph::source_of(PortRef to) const {
    const auto it = m_connections.find(to);
    if (it == m_connections.end()) { return std::nullopt; }
    return it->second;
}

std::set<const void*> Graph::exclusive_resources() const {
    std::set<const void*> resources;
    for (const auto& [id, node] : m_nodes) {
        resources.insert(node.get());
        for (const void* resource : node->exclusive_resources()) { resources.insert(resource); }
    }
    return resources;
}

std::optional<NodeId> Graph::add_node(std::shared_ptr<Node> node) {
    if (!node) { return std::nullopt; }

    std::set<const void*> in_use = exclusive_resources();
    if (!in_use.insert(node.get()).second) { return std::nullopt; }
    for (const void* resource : node->exclusive_resources()) {
        if (!in_use.insert(resource).second) { return std::nullopt; }
    }

    if (m_spec.m_max_block_size > 0) { node->prepare(m_spec); }
    const NodeId id{m_next_id++};
    m_nodes.emplace(id, std::move(node));
    m_dirty = true;
    return id;
}

bool Graph::remove_node(NodeId id) {
    if (id == k_output_id) { return false; }
    if (m_nodes.erase(id) == 0) { return false; }

    std::erase_if(m_connections, [id](const auto& entry) {
        const auto& [to, from] = entry;
        return to.m_node == id || from.m_node == id;
    });
    m_dirty = true;
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
            if (from.m_node == n) { stack.push_back(to.m_node); }
        }
    }
    return false;
}

bool Graph::connect(PortRef from, PortRef to) {
    if (!exists(from.m_node) || !exists(to.m_node) || from.m_node == to.m_node) { return false; }

    const PortLayout from_ports = m_nodes.at(from.m_node)->ports();
    const PortLayout to_ports = m_nodes.at(to.m_node)->ports();
    if (from.m_port >= from_ports.m_outputs.size() || to.m_port >= to_ports.m_inputs.size()) {
        return false;
    }
    if (from_ports.m_outputs[from.m_port] != to_ports.m_inputs[to.m_port]) { return false; }

    if (m_connections.contains(to)) { return false; }  // an input has exactly one source
    if (path_exists(to.m_node, from.m_node)) { return false; }

    m_connections.emplace(to, from);
    m_dirty = true;
    return true;
}

bool Graph::disconnect(PortRef to) {
    if (m_connections.erase(to) == 0) { return false; }
    m_dirty = true;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Prepare / compile / publish
// ─────────────────────────────────────────────────────────────────────────────

void Graph::prepare(const ProcessSpec& spec) {
    m_spec = spec;
    for (auto& [id, node] : m_nodes) { node->prepare(spec); }
    commit();
}

std::optional<std::vector<NodeId>> Graph::sort_graph_dfs() const {
    enum class VisitState : uint8_t { InProgress, Done };
    struct Frame {
        NodeId m_node;
        size_t m_num_inputs;
        uint32_t m_next_port = 0;
    };

    std::map<NodeId, VisitState> state;
    std::vector<NodeId> order;
    std::vector<Frame> stack;

    const auto push = [&](NodeId id) {
        state[id] = VisitState::InProgress;
        stack.push_back({.m_node = id, .m_num_inputs = m_nodes.at(id)->ports().m_inputs.size()});
    };
    push(k_output_id);

    while (!stack.empty()) {
        Frame& frame = stack.back();

        if (frame.m_next_port == frame.m_num_inputs) {  // every source of this node is in `order`
            state[frame.m_node] = VisitState::Done;
            order.push_back(frame.m_node);
            stack.pop_back();
            continue;
        }

        const auto source =
            m_connections.find(PortRef{.m_node = frame.m_node, .m_port = frame.m_next_port++});
        if (source == m_connections.end()) { continue; }  // unconnected input

        const NodeId source_node = source->second.m_node;
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
    compiled.m_max_block_size = m_spec.m_max_block_size;
    compiled.m_buffers = std::make_unique<GraphBuffers>();
    auto& buffers = compiled.m_buffers->m_buffers;

    buffers.emplace_back();  // k_zeros_buffer; sized after the loop, once the widest input is known
    size_t max_input_channels = 0;
    size_t max_inputs = 0;
    size_t max_outputs = 0;

    // `order` has every node after its sources, so a source's buffer always
    // exists by the time an input looks it up.
    std::map<PortRef, BufferIndex> output_buffers;  // output port -> its buffer
    for (const NodeId id : *order) {
        const std::shared_ptr<Node>& node = m_nodes.at(id);
        const PortLayout layout = node->ports();
        ProcessingStep step{.m_node = node.get(), .m_ports = layout};

        for (uint32_t port = 0; port < layout.m_inputs.size(); ++port) {
            const auto source = m_connections.find(PortRef{.m_node = id, .m_port = port});
            step.m_input_buffers.push_back(
                source == m_connections.end() ? k_zeros_buffer : output_buffers.at(source->second));
            max_input_channels = std::max(max_input_channels, layout.m_inputs[port]);
        }
        for (uint32_t port = 0; port < layout.m_outputs.size(); ++port) {
            const auto index = static_cast<BufferIndex>(buffers.size());
            buffers.emplace_back(layout.m_outputs[port], m_spec.m_max_block_size);
            output_buffers.emplace(PortRef{.m_node = id, .m_port = port}, index);
            step.m_output_buffers.push_back(index);
        }
        max_inputs = std::max(max_inputs, layout.m_inputs.size());
        max_outputs = std::max(max_outputs, layout.m_outputs.size());

        compiled.m_nodes.push_back(node);
        compiled.m_steps.push_back(std::move(step));
    }

    buffers[k_zeros_buffer] = thl::core::Buffer<float>(max_input_channels, m_spec.m_max_block_size);
    compiled.m_buffers->m_input_views.resize(max_inputs);
    compiled.m_buffers->m_output_views.resize(max_outputs);

    const auto source = m_connections.find(PortRef{.m_node = k_output_id, .m_port = 0});
    if (source != m_connections.end()) {
        compiled.m_output_buffer = output_buffers.at(source->second);
    }

    return compiled;
}

bool Graph::commit() {
    auto compiled = compile();
    if (!compiled) { return false; }

    // The previous snapshot is retired and freed later, on this thread, once
    // the audio thread has left it.
    m_compiled_graph.replace([&](CompiledGraph& g) { g = std::move(*compiled); });
    m_dirty = false;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Processing
// ─────────────────────────────────────────────────────────────────────────────

void Graph::register_audio_thread() const {
    m_compiled_graph.register_reader_thread();
}

void Graph::process(thl::core::BufferView output) {
    const auto scope = m_compiled_graph.read_scope();
    const CompiledGraph& g = scope.data();
    const size_t n = output.get_num_samples();

    // Silence until prepare() is called, or if the block is longer than prepare() allowed.
    if (g.m_max_block_size == 0 || n > g.m_max_block_size) {
        for (size_t ch = 0; ch < output.get_num_channels(); ++ch) {
            std::fill_n(output.get_write_pointer(ch), n, 0.0f);
        }
        return;
    }
    GraphBuffers& b = *g.m_buffers;

    for (const ProcessingStep& step : g.m_steps) {
        for (size_t p = 0; p < step.m_input_buffers.size(); ++p) {
            const auto& buffer = b.m_buffers[step.m_input_buffers[p]];
            b.m_input_views[p] = thl::core::ConstBufferView(buffer.get_array_of_read_pointers(),
                                                            step.m_ports.m_inputs[p],
                                                            n);
        }
        for (size_t p = 0; p < step.m_output_buffers.size(); ++p) {
            auto& buffer = b.m_buffers[step.m_output_buffers[p]];
            b.m_output_views[p] = thl::core::BufferView(buffer.get_array_of_write_pointers(),
                                                        step.m_ports.m_outputs[p],
                                                        n);
        }
        step.m_node->process(
            {.m_inputs = std::span(b.m_input_views).first(step.m_input_buffers.size()),
             .m_outputs = std::span(b.m_output_views).first(step.m_output_buffers.size()),
             .m_num_frames = n});
    }

    const auto& out_buf = b.m_buffers[g.m_output_buffer];
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
