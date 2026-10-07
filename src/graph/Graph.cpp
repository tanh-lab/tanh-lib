#include <tanh/core/Buffer.h>
#include <tanh/core/BufferView.h>
#include <tanh/dsp/BaseProcessor.h>
#include <tanh/graph/Graph.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace thl::graph {

// ─────────────────────────────────────────────────────────────────────────────
// Node: what the graph keeps per node. graph_input() and graph_output() have no
// processor.
// ─────────────────────────────────────────────────────────────────────────────

struct Graph::Node {
    std::unique_ptr<thl::dsp::BaseProcessor> m_processor;
    MixMode m_mix_mode = MixMode::Sum;
};

// ─────────────────────────────────────────────────────────────────────────────
// The compiled version of the graph. Built on the control thread, then owned by
// the audio thread until it hands it back through m_returned.
// ─────────────────────────────────────────────────────────────────────────────

struct Graph::ProcessingStep {
    thl::dsp::BaseProcessor* m_processor = nullptr;
    MixMode m_mix_mode = MixMode::Sum;
    std::vector<BufferIndex> m_sources;
    BufferIndex m_buffer = 0;
};

struct Graph::CompiledGraph {
    uint64_t m_generation = 0;
    std::vector<ProcessingStep> m_steps;  // in processing order
    std::vector<thl::core::Buffer<float>> m_buffers;
    std::optional<BufferIndex> m_input_buffer;  // empty if graph_input() reaches no output
    std::vector<BufferIndex> m_output_sources;
    MixMode m_output_mix_mode = MixMode::Sum;
    size_t m_num_channels = 0;
    size_t m_max_block_size = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

Graph::Graph() {
    m_nodes.emplace(k_output_id, std::make_unique<Node>());
    m_nodes.emplace(k_input_id, std::make_unique<Node>());
}

Graph::~Graph() {
    free_all_compiled();
}

// ─────────────────────────────────────────────────────────────────────────────
// Inspection
// ─────────────────────────────────────────────────────────────────────────────

thl::dsp::BaseProcessor* Graph::node(NodeId id) {
    const auto it = m_nodes.find(id);
    return it == m_nodes.end() ? nullptr : it->second->m_processor.get();
}

const thl::dsp::BaseProcessor* Graph::node(NodeId id) const {
    const auto it = m_nodes.find(id);
    return it == m_nodes.end() ? nullptr : it->second->m_processor.get();
}

std::vector<NodeId> Graph::nodes() const {
    std::vector<NodeId> ids;
    ids.reserve(m_nodes.size());
    for (const auto& [id, node] : m_nodes) { ids.push_back(id); }
    return ids;
}

std::vector<Connection> Graph::connections() const {
    return {m_connections.begin(), m_connections.end()};
}

std::vector<NodeId> Graph::sources_of(NodeId id) const {
    std::vector<NodeId> sources;
    for (const Connection& connection : m_connections) {
        if (connection.m_to == id) { sources.push_back(connection.m_from); }
    }
    return sources;
}

// ─────────────────────────────────────────────────────────────────────────────
// Editing
// ─────────────────────────────────────────────────────────────────────────────

std::optional<NodeId> Graph::add_node(std::unique_ptr<thl::dsp::BaseProcessor> processor) {
    if (!processor) { return std::nullopt; }

    if (m_max_block_size > 0) {
        processor->prepare(m_sample_rate, m_max_block_size, m_num_channels);
    }
    const NodeId id{m_next_id++};
    m_nodes.emplace(id, std::make_unique<Node>(Node{.m_processor = std::move(processor)}));
    m_dirty = true;
    return id;
}

bool Graph::remove_node(NodeId id) {
    if (id == k_input_id || id == k_output_id) { return false; }
    const auto it = m_nodes.find(id);
    if (it == m_nodes.end()) { return false; }

    // Versions already handed to the audio thread may still run this node.
    m_retired_nodes.push_back(
        {.m_node = std::move(it->second), .m_last_generation = m_next_generation - 1});
    m_nodes.erase(it);

    std::erase_if(m_connections, [id](const Connection& connection) {
        return connection.m_from == id || connection.m_to == id;
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
        for (const Connection& connection : m_connections) {
            if (connection.m_from == n) { stack.push_back(connection.m_to); }
        }
    }
    return false;
}

bool Graph::connect(NodeId from, NodeId to) {
    if (!exists(from) || !exists(to) || from == to) { return false; }
    if (from == k_output_id || to == k_input_id) { return false; }
    if (path_exists(to, from)) { return false; }

    if (!m_connections.insert({.m_from = from, .m_to = to}).second) { return false; }
    m_dirty = true;
    return true;
}

bool Graph::disconnect(NodeId from, NodeId to) {
    if (m_connections.erase({.m_from = from, .m_to = to}) == 0) { return false; }
    m_dirty = true;
    return true;
}

bool Graph::set_mix_mode(NodeId id, MixMode mode) {
    const auto it = m_nodes.find(id);
    if (it == m_nodes.end() || id == k_input_id) { return false; }

    if (it->second->m_mix_mode != mode) {
        it->second->m_mix_mode = mode;
        m_dirty = true;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Prepare / compile / publish
// ─────────────────────────────────────────────────────────────────────────────

void Graph::prepare(const double& sample_rate,
                    const size_t& samples_per_block,
                    const size_t& num_channels) {
    m_sample_rate = sample_rate;
    m_max_block_size = samples_per_block;
    m_num_channels = num_channels;
    for (auto& [id, node] : m_nodes) {
        if (node->m_processor) {
            node->m_processor->prepare(sample_rate, samples_per_block, num_channels);
        }
    }

    // process() is not running, so nothing has to be handed over.
    free_all_compiled();
    auto compiled = compile();
    if (!compiled) { return; }
    compiled->m_generation = m_next_generation++;
    m_live_generations.insert(compiled->m_generation);
    m_active = compiled.release();
    m_dirty = false;
}

std::optional<std::vector<NodeId>> Graph::sort_graph_dfs() const {
    enum class VisitState : uint8_t { InProgress, Done };
    struct Frame {
        NodeId m_node;
        std::vector<NodeId> m_sources;
        size_t m_next_source = 0;
    };

    std::map<NodeId, VisitState> state;
    std::vector<NodeId> order;
    std::vector<Frame> stack;

    const auto push = [&](NodeId id) {
        state[id] = VisitState::InProgress;
        stack.push_back({.m_node = id, .m_sources = sources_of(id)});
    };
    push(k_output_id);

    while (!stack.empty()) {
        Frame& frame = stack.back();

        if (frame.m_next_source == frame.m_sources.size()) {  // every source is in `order`
            state[frame.m_node] = VisitState::Done;
            order.push_back(frame.m_node);
            stack.pop_back();
            continue;
        }

        const NodeId source = frame.m_sources[frame.m_next_source++];
        const auto it = state.find(source);
        if (it == state.end()) {
            push(source);  // may invalidate `frame`; it is not used again this iteration
        } else if (it->second == VisitState::InProgress) {
            return std::nullopt;  // back on our own path: cycle
        }
    }
    return order;
}

std::unique_ptr<Graph::CompiledGraph> Graph::compile() const {
    const auto order = sort_graph_dfs();
    if (!order) { return nullptr; }

    auto compiled = std::make_unique<CompiledGraph>();
    compiled->m_num_channels = m_num_channels;
    compiled->m_max_block_size = m_max_block_size;

    // `order` has every node after its sources, so a source's buffer always
    // exists by the time a node looks it up.
    std::map<NodeId, BufferIndex> buffer_of;
    for (const NodeId id : *order) {
        const Node& node = *m_nodes.at(id);
        std::vector<BufferIndex> sources;
        for (const NodeId source : sources_of(id)) { sources.push_back(buffer_of.at(source)); }

        if (id == k_output_id) {
            compiled->m_output_sources = std::move(sources);
            compiled->m_output_mix_mode = node.m_mix_mode;
            continue;
        }

        const auto index = static_cast<BufferIndex>(compiled->m_buffers.size());
        compiled->m_buffers.emplace_back(m_num_channels, m_max_block_size);
        buffer_of.emplace(id, index);

        if (id == k_input_id) {
            compiled->m_input_buffer = index;
            continue;
        }
        compiled->m_steps.push_back({.m_processor = node.m_processor.get(),
                                     .m_mix_mode = node.m_mix_mode,
                                     .m_sources = std::move(sources),
                                     .m_buffer = index});
    }
    return compiled;
}

bool Graph::commit() {
    collect_garbage();
    auto compiled = compile();
    if (!compiled) { return false; }

    compiled->m_generation = m_next_generation++;
    publish(std::move(compiled));
    m_dirty = false;
    return true;
}

void Graph::publish(std::unique_ptr<CompiledGraph> compiled) {
    m_live_generations.insert(compiled->m_generation);
    CompiledGraph* stale = m_pending.exchange(compiled.release(), std::memory_order_acq_rel);

    // The audio thread never picked this one up, so it can be freed right away.
    if (stale != nullptr) {
        m_live_generations.erase(stale->m_generation);
        delete stale;
    }
}

void Graph::collect_garbage() {
    CompiledGraph* returned = nullptr;
    while (m_returned.try_pop(returned)) {
        m_live_generations.erase(returned->m_generation);
        delete returned;
    }

    const uint64_t oldest_live = m_live_generations.empty() ? std::numeric_limits<uint64_t>::max()
                                                            : *m_live_generations.begin();
    std::erase_if(m_retired_nodes, [oldest_live](const RetiredNode& retired) {
        return retired.m_last_generation < oldest_live;
    });
}

void Graph::free_all_compiled() {
    delete m_pending.exchange(nullptr, std::memory_order_acq_rel);
    delete m_active;
    m_active = nullptr;

    CompiledGraph* returned = nullptr;
    while (m_returned.try_pop(returned)) { delete returned; }

    m_live_generations.clear();
    m_retired_nodes.clear();
}

// ─────────────────────────────────────────────────────────────────────────────
// Processing
// ─────────────────────────────────────────────────────────────────────────────

void Graph::take_pending() {
    if (m_pending.load(std::memory_order_acquire) == nullptr) { return; }

    // If the control thread has not emptied m_returned yet, switch at a later block instead.
    if (m_active != nullptr && !m_returned.try_push(m_active)) { return; }
    m_active = m_pending.exchange(nullptr, std::memory_order_acq_rel);
}

void Graph::mix_sources(const CompiledGraph& compiled,
                        std::span<const BufferIndex> sources,
                        MixMode mode,
                        thl::core::BufferView destination) {
    const size_t n = destination.get_num_samples();
    const float scale = 1.0f / static_cast<float>(sources.size());

    for (size_t ch = 0; ch < destination.get_num_channels(); ++ch) {
        float* out = destination.get_write_pointer(ch);
        if (sources.empty()) {
            std::fill_n(out, n, 0.0f);
            continue;
        }

        std::copy_n(compiled.m_buffers[sources[0]].get_read_pointer(ch), n, out);
        for (size_t s = 1; s < sources.size(); ++s) {
            const float* in = compiled.m_buffers[sources[s]].get_read_pointer(ch);
            for (size_t i = 0; i < n; ++i) { out[i] += in[i]; }
        }
        if (mode == MixMode::Average) {
            for (size_t i = 0; i < n; ++i) { out[i] *= scale; }
        }
    }
}

void Graph::process(thl::core::BufferView buffer, uint32_t /*modulation_offset*/) {
    take_pending();
    CompiledGraph* compiled = m_active;
    const size_t n = buffer.get_num_samples();

    // Silence until prepare() is called, or if the buffer does not fit what prepare() was given.
    if (compiled == nullptr || n > compiled->m_max_block_size ||
        buffer.get_num_channels() != compiled->m_num_channels) {
        for (size_t ch = 0; ch < buffer.get_num_channels(); ++ch) {
            std::fill_n(buffer.get_write_pointer(ch), n, 0.0f);
        }
        return;
    }

    if (compiled->m_input_buffer) {
        auto& input = compiled->m_buffers[*compiled->m_input_buffer];
        for (size_t ch = 0; ch < compiled->m_num_channels; ++ch) {
            std::copy_n(buffer.get_read_pointer(ch), n, input.get_write_pointer(ch));
        }
    }

    for (const ProcessingStep& step : compiled->m_steps) {
        auto& own = compiled->m_buffers[step.m_buffer];
        const thl::core::BufferView view(own.get_array_of_write_pointers(),
                                         compiled->m_num_channels,
                                         n);
        mix_sources(*compiled, step.m_sources, step.m_mix_mode, view);
        step.m_processor->process_modulated(view);
    }

    mix_sources(*compiled, compiled->m_output_sources, compiled->m_output_mix_mode, buffer);
}

}  // namespace thl::graph
