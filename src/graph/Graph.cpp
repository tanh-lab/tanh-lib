#include "tanh/graph/Graph.h"
#include <tanh/core/Buffer.h>

#include <algorithm>
#include <optional>
#include <queue>

namespace thl::graph {

// ─────────────────────────────────────────────────────────────────────────────
// The compiled snapshot. Plain data: built on the control thread, then only
// read by the audio thread — except Scratch, which the audio thread writes
// into every block and reaches through a pointer (see RCU::replace()).
// ─────────────────────────────────────────────────────────────────────────────

namespace {

/// The graph's own input/output. process() is a no-op: Graph::process()
/// fills the input endpoint's output buffer and reads the output endpoint's
/// input buffer directly.
class EndpointNode final : public Node {
public:
    explicit EndpointNode(PortLayout layout) : m_layout(std::move(layout)) {}
    PortLayout ports() const override { return m_layout; }
    void process(const ProcessContext&) TANH_NONBLOCKING_FUNCTION override {}

private:
    PortLayout m_layout;
};

}  // namespace

namespace detail {

using BufferIndex = uint32_t;

/// "Add source buffer into destination buffer" — how fan-in is summed.
struct MixOp {
    BufferIndex source;  // an output-port buffer
    BufferIndex dest;    // an input-port buffer
};

struct Step {
    Node* node = nullptr;
    std::vector<BufferIndex> input_buffers;   // one per input port
    std::vector<BufferIndex> output_buffers;  // one per output port
    std::vector<MixOp> mixes;                 // what feeds those input ports
};

struct StepViews {
    std::vector<thl::core::ConstBufferView> inputs;
    std::vector<thl::core::BufferView> outputs;
};

struct Scratch {
    std::vector<thl::core::Buffer<float>> buffers;
    std::vector<StepViews> views;  // parallel to CompiledGraph::steps
};

}  // namespace detail

using namespace detail;

namespace {

void clear(thl::core::Buffer<float>& b, size_t n) TANH_NONBLOCKING_FUNCTION {
    for (size_t ch = 0; ch < b.get_num_channels(); ++ch) {
        std::fill_n(b.get_write_pointer(ch), n, 0.0f);
    }
}

void add(const thl::core::Buffer<float>& src, thl::core::Buffer<float>& dst, size_t n)
    TANH_NONBLOCKING_FUNCTION {
    const size_t channels = std::min(src.get_num_channels(), dst.get_num_channels());
    for (size_t ch = 0; ch < channels; ++ch) {
        const float* s = src.get_read_pointer(ch);
        float* d = dst.get_write_pointer(ch);
        for (size_t i = 0; i < n; ++i) { d[i] += s[i]; }
    }
}

}  // namespace

struct CompiledGraph {
    std::vector<std::shared_ptr<Node>> keep_alive;  // nodes live as long as the snapshot
    std::vector<Step> steps;                        // topological order
    std::unique_ptr<Scratch> scratch;               // null = nothing published yet
    BufferIndex graph_input_buffer = 0;
    BufferIndex graph_output_buffer = 0;
    size_t max_block_size = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

Graph::Graph(size_t num_input_channels, size_t num_output_channels)
    : m_compiled_graph(CompiledGraph{}) {
    m_nodes.emplace(k_input_id,
                    std::make_shared<EndpointNode>(PortLayout{{}, {num_input_channels}}));
    m_nodes.emplace(k_output_id,
                    std::make_shared<EndpointNode>(PortLayout{{num_output_channels}, {}}));
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
    if (id == k_input_id || id == k_output_id) { return false; }
    if (m_nodes.erase(id) == 0) { return false; }

    std::erase_if(m_connections,
                  [id](const Connection& c) { return c.from.node == id || c.to.node == id; });
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
        for (const auto& c : m_connections) {
            if (c.from.node == n) { stack.push_back(c.to.node); }
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

    const Connection c{.from=from, .to=to};
    if (std::ranges::find(m_connections, c) != m_connections.end()) { return false; }

    if (path_exists(to.node, from.node)) { return false; }

    m_connections.push_back(c);
    return true;
}

bool Graph::disconnect(PortRef from, PortRef to) {
    return std::erase(m_connections, Connection{.from=from, .to=to}) > 0;
}

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

void Graph::register_audio_thread() const { m_compiled_graph.register_reader_thread(); }

namespace {

/// Kahn's algorithm. nullopt if the model contains a cycle.
std::optional<std::vector<NodeId>> topological_order(const std::vector<NodeId>& nodes,
                                                     const std::vector<Connection>& connections) {
    std::map<NodeId, size_t> in_degree;
    for (NodeId n : nodes) { in_degree[n] = 0; }
    for (const auto& c : connections) { ++in_degree[c.to.node]; }

    std::queue<NodeId> ready;
    for (NodeId n : nodes) {
        if (in_degree[n] == 0) { ready.push(n); }
    }

    std::vector<NodeId> order;
    while (!ready.empty()) {
        const NodeId n = ready.front();
        ready.pop();
        order.push_back(n);
        for (const auto& c : connections) {
            if (c.from.node == n && --in_degree[c.to.node] == 0) { ready.push(c.to.node); }
        }
    }
    if (order.size() != nodes.size()) { return std::nullopt; }
    return order;
}

}  // namespace

bool Graph::commit() {
    // 1. Validate.
    std::vector<NodeId> ids;
    for (const auto& [id, entry] : m_nodes) { ids.push_back(id); }
    const auto order = topological_order(ids, m_connections);
    if (!order) { return false; }

    if (m_spec.max_block_size == 0) { return true; }  // valid; prepare() will publish

    // 2. Allocate one buffer per port.
    auto compiled = std::make_unique<CompiledGraph>();
    compiled->scratch = std::make_unique<Scratch>();
    compiled->max_block_size = m_spec.max_block_size;
    auto& buffers = compiled->scratch->buffers;

    std::map<NodeId, std::vector<BufferIndex>> in_buf, out_buf;
    for (NodeId id : *order) {
        const PortLayout layout = m_nodes.at(id)->ports();
        for (size_t ch : layout.inputs) {
            in_buf[id].push_back(static_cast<BufferIndex>(buffers.size()));
            buffers.emplace_back(ch, m_spec.max_block_size);
        }
        for (size_t ch : layout.outputs) {
            out_buf[id].push_back(static_cast<BufferIndex>(buffers.size()));
            buffers.emplace_back(ch, m_spec.max_block_size);
        }
    }

    // 3. Emit one step per node, in topological order.
    for (NodeId id : *order) {
        Step step;
        step.node = m_nodes.at(id).get();
        step.input_buffers = in_buf[id];
        step.output_buffers = out_buf[id];
        for (const auto& c : m_connections) {
            if (c.to.node == id) {
                step.mixes.push_back({.source=out_buf[c.from.node][c.from.port], .dest=in_buf[id][c.to.port]});
            }
        }
        compiled->keep_alive.push_back(m_nodes.at(id));
        compiled->steps.push_back(std::move(step));

        StepViews views;  // pre-size so the audio thread only assigns
        views.inputs.resize(in_buf[id].size());
        views.outputs.resize(out_buf[id].size());
        compiled->scratch->views.push_back(std::move(views));
    }
    compiled->graph_input_buffer = out_buf[k_input_id][0];
    compiled->graph_output_buffer = in_buf[k_output_id][0];

    // 4. Publish. The previous snapshot is retired and freed later, on this
    //    thread, once the audio thread has left it.
    m_compiled_graph.replace([&](CompiledGraph& g) { g = std::move(*compiled); });
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Processing
// ─────────────────────────────────────────────────────────────────────────────

void Graph::process(thl::core::ConstBufferView input, thl::core::BufferView output) {
    const auto scope = m_compiled_graph.read_scope();
    const CompiledGraph& g = scope.data();
    const size_t n = output.get_num_samples();

    if (g.scratch == nullptr || n > g.max_block_size) {
        for (size_t ch = 0; ch < output.get_num_channels(); ++ch) {
            std::fill_n(output.get_write_pointer(ch), n, 0.0f);
        }
        return;
    }
    Scratch& s = *g.scratch;

    // External input -> the input endpoint's output buffer.
    auto& in_buf = s.buffers[g.graph_input_buffer];
    for (size_t ch = 0; ch < in_buf.get_num_channels(); ++ch) {
        float* d = in_buf.get_write_pointer(ch);
        if (ch < input.get_num_channels() && input.get_num_samples() >= n) {
            std::copy_n(input.get_read_pointer(ch), n, d);
        } else {
            std::fill_n(d, n, 0.0f);
        }
    }

    // Run every step: sum what feeds each input port, then process.
    for (size_t i = 0; i < g.steps.size(); ++i) {
        const Step& step = g.steps[i];
        StepViews& v = s.views[i];

        for (BufferIndex b : step.input_buffers) { clear(s.buffers[b], n); }
        for (const MixOp& m : step.mixes) { add(s.buffers[m.source], s.buffers[m.dest], n); }

        for (size_t p = 0; p < step.input_buffers.size(); ++p) {
            const auto& b = s.buffers[step.input_buffers[p]];
            v.inputs[p] = thl::core::ConstBufferView(b).sub_block(0, n);
        }
        for (size_t p = 0; p < step.output_buffers.size(); ++p) {
            auto& b = s.buffers[step.output_buffers[p]];
            v.outputs[p] = thl::core::BufferView(b).sub_block(0, n);
        }
        step.node->process({.inputs=v.inputs, .outputs=v.outputs, .num_frames=n});
    }

    // The output endpoint's input buffer -> external output.
    const auto& out_buf = s.buffers[g.graph_output_buffer];
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
