#include <tanh/core/Buffer.h>
#include <tanh/core/BufferView.h>
#include <tanh/dsp/BaseProcessor.h>
#include <tanh/dsp/utils/DelayProcessor.h>
#include <tanh/dsp/utils/SmoothedValue.h>
#include <tanh/graph/Graph.h>

#include <algorithm>
#include <atomic>
#include <cmath>
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
    static constexpr double k_ramp_seconds = 0.02;

    std::unique_ptr<thl::dsp::BaseProcessor> m_processor;
    std::atomic<InputMix> m_input_mix{InputMix::Average};
    thl::dsp::utils::SmoothedValue m_input_mix_smoother;
    std::atomic<float> m_wet_mix{1.0f};
    thl::dsp::utils::SmoothedValue m_wet_mix_smoother;
    std::unique_ptr<thl::dsp::utils::DelayProcessor> m_dry_delay;
};

// ─────────────────────────────────────────────────────────────────────────────
// The compiled version of the graph. Built on the control thread, then owned by
// the audio thread until it hands it back through m_returned.
// ─────────────────────────────────────────────────────────────────────────────

struct Graph::Source {
    BufferIndex m_buffer = 0;
    thl::dsp::utils::DelayProcessor m_delay;
};

struct Graph::ProcessingStep {
    Node* m_node = nullptr;
    std::vector<Source> m_sources;
    BufferIndex m_buffer = 0;
    thl::dsp::utils::DelayProcessor* m_dry_delay = nullptr;
};

struct Graph::CompiledGraph {
    uint64_t m_generation = 0;
    std::vector<ProcessingStep> m_steps;  // in processing order
    std::vector<thl::core::Buffer<float>> m_buffers;
    std::vector<uint32_t> m_path_latencies;     // per buffer, set by apply_latencies()
    uint32_t m_max_latency = 0;                 // of the output
    thl::core::Buffer<float> m_dry;             // scratch for steps that blend dry and processed
    std::optional<BufferIndex> m_input_buffer;  // empty if graph_input() reaches no output
    std::vector<Source> m_output_sources;
    Node* m_output_node = nullptr;
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

std::optional<float> Graph::wet_mix(NodeId id) const {
    if (id == k_input_id || id == k_output_id) { return std::nullopt; }
    const auto it = m_nodes.find(id);
    if (it == m_nodes.end()) { return std::nullopt; }
    return it->second->m_wet_mix.load(std::memory_order_relaxed);
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
    return insert_node(std::move(processor), true);
}

std::optional<NodeId> Graph::add_prepared_node(std::unique_ptr<thl::dsp::BaseProcessor> processor,
                                               double sample_rate,
                                               size_t samples_per_block,
                                               size_t num_channels) {
    const bool matches = sample_rate == m_sample_rate && samples_per_block == m_max_block_size &&
                         num_channels == m_num_channels;
    return insert_node(std::move(processor), !matches);
}

std::optional<NodeId> Graph::insert_node(std::unique_ptr<thl::dsp::BaseProcessor> processor,
                                         bool prepare_processor) {
    if (!processor) { return std::nullopt; }

    auto node = std::make_unique<Node>();
    node->m_processor = std::move(processor);
    if (m_max_block_size > 0) { prepare_node(*node, prepare_processor); }

    const NodeId id{m_next_id++};
    m_nodes.emplace(id, std::move(node));
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

bool Graph::set_input_mix(NodeId id, InputMix input_mix) {
    const auto it = m_nodes.find(id);
    if (it == m_nodes.end() || id == k_input_id) { return false; }

    it->second->m_input_mix.store(input_mix, std::memory_order_relaxed);
    return true;
}

bool Graph::set_wet_mix(NodeId id, float wet_mix) {
    if (id == k_input_id || id == k_output_id || std::isnan(wet_mix)) { return false; }
    const auto it = m_nodes.find(id);
    if (it == m_nodes.end()) { return false; }

    it->second->m_wet_mix.store(std::clamp(wet_mix, 0.0f, 1.0f), std::memory_order_relaxed);
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
    m_buffer_pool.clear();  // sized for the previous prepare()
    for (auto& [id, node] : m_nodes) { prepare_node(*node); }
    set_max_latency_samples(0);
    rebuild();

    if (switch_to_pending()) { set_latency_samples(apply_latencies(*m_active)); }
    m_fade.reset(m_sample_rate, k_fade_seconds);
    m_fade.set_current_and_target_value(1.0f);
    m_fade_samples = static_cast<size_t>(k_fade_seconds * m_sample_rate) + 1;
    m_fading_out = false;
}

void Graph::prepare_node(Node& node, bool prepare_processor) const {
    if (prepare_processor && node.m_processor) {
        node.m_processor->prepare(m_sample_rate, m_max_block_size, m_num_channels);
    }
    if (node.m_dry_delay) {
        node.m_dry_delay->prepare(m_sample_rate, m_max_block_size, m_num_channels);
    }
    // Start at the current values instead of ramping to them.
    const bool average = node.m_input_mix.load(std::memory_order_relaxed) == InputMix::Average;
    node.m_input_mix_smoother.reset(m_sample_rate, Node::k_ramp_seconds);
    node.m_input_mix_smoother.set_current_and_target_value(average ? 1.0f : 0.0f);

    node.m_wet_mix_smoother.reset(m_sample_rate, Node::k_ramp_seconds);
    node.m_wet_mix_smoother.set_current_and_target_value(
        node.m_wet_mix.load(std::memory_order_relaxed));
}

void Graph::prepare_delays(std::span<Source> sources,
                           std::span<const uint32_t> max_path_latencies) const {
    for (Source& source : sources) {
        // A source waits at most for the slowest of the other sources.
        uint32_t max_delay = 0;
        for (const Source& other : sources) {
            if (&other != &source) {
                max_delay = std::max(max_delay, max_path_latencies[other.m_buffer]);
            }
        }
        source.m_delay.set_max_delay_samples(max_delay);
        source.m_delay.prepare(m_sample_rate, m_max_block_size, m_num_channels);
    }
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

std::unique_ptr<Graph::CompiledGraph> Graph::compile() {
    const auto order = sort_graph_dfs();
    if (!order) { return nullptr; }

    auto compiled = std::make_unique<CompiledGraph>();
    compiled->m_num_channels = m_num_channels;
    compiled->m_max_block_size = m_max_block_size;
    compiled->m_dry = take_buffer();

    // `order` has every node after its sources, so a source's buffer always
    // exists by the time a node looks it up.
    std::map<NodeId, BufferIndex> buffer_of;
    std::vector<uint32_t> max_path_latencies;  // per buffer
    for (const NodeId id : *order) {
        Node& node = *m_nodes.at(id);
        std::vector<Source> sources;
        uint32_t max_path_latency = 0;
        for (const NodeId source : sources_of(id)) {
            const BufferIndex buffer = buffer_of.at(source);
            sources.push_back({.m_buffer = buffer});
            max_path_latency = std::max(max_path_latency, max_path_latencies[buffer]);
        }
        prepare_delays(sources, max_path_latencies);

        if (id == k_output_id) {
            compiled->m_output_sources = std::move(sources);
            compiled->m_output_node = &node;
            compiled->m_max_latency = max_path_latency;
            continue;
        }

        const uint32_t max = node.m_processor ? node.m_processor->max_latency_samples() : 0;
        const auto index = static_cast<BufferIndex>(compiled->m_buffers.size());
        compiled->m_buffers.push_back(take_buffer());
        max_path_latencies.push_back(max_path_latency + max);
        buffer_of.emplace(id, index);

        if (id == k_input_id) {
            compiled->m_input_buffer = index;
            continue;
        }
        const bool keep_delay = node.m_dry_delay && node.m_dry_delay->max_delay_samples() == max;
        if (!keep_delay) {
            if (node.m_dry_delay) {
                m_retired_delays.push_back({.m_delay = std::move(node.m_dry_delay),
                                            .m_last_generation = m_next_generation - 1});
            }
            node.m_dry_delay = std::make_unique<thl::dsp::utils::DelayProcessor>(max);
            node.m_dry_delay->prepare(m_sample_rate, m_max_block_size, m_num_channels);
        }
        compiled->m_steps.push_back({.m_node = &node,
                                     .m_sources = std::move(sources),
                                     .m_buffer = index,
                                     .m_dry_delay = node.m_dry_delay.get()});
    }
    compiled->m_path_latencies.assign(compiled->m_buffers.size(), 0);
    return compiled;
}

bool Graph::commit() {
    if (!m_dirty && m_published) {  // nothing to hand over: no fade
        collect_garbage();
        return true;
    }
    return rebuild();
}

bool Graph::rebuild() {
    collect_garbage();
    auto compiled = compile();
    if (!compiled) { return false; }

    compiled->m_generation = m_next_generation++;
    if (compiled->m_max_latency > max_latency_samples()) {
        set_max_latency_samples(compiled->m_max_latency);
    }
    publish(std::move(compiled));
    m_dirty = false;
    return true;
}

void Graph::publish(std::unique_ptr<CompiledGraph> compiled) {
    m_live_generations.insert(compiled->m_generation);
    m_published = true;
    CompiledGraph* stale = m_pending.exchange(compiled.release(), std::memory_order_acq_rel);

    // The audio thread never picked this one up, so it can be freed right away.
    if (stale != nullptr) {
        m_live_generations.erase(stale->m_generation);
        recycle(stale);
    }
}

void Graph::recycle(CompiledGraph* compiled) {
    // Keep its buffers for the next compile() if they still fit prepare(); enough
    // for every node, the dry scratch and the input.
    const size_t limit = m_nodes.size() + 2;
    const auto keep = [&](thl::core::Buffer<float>& buffer) {
        if (m_buffer_pool.size() < limit && buffer.get_num_channels() == m_num_channels &&
            buffer.get_num_samples() == m_max_block_size) {
            m_buffer_pool.push_back(std::move(buffer));
        }
    };
    for (auto& buffer : compiled->m_buffers) { keep(buffer); }
    keep(compiled->m_dry);
    delete compiled;
}

thl::core::Buffer<float> Graph::take_buffer() {
    if (m_buffer_pool.empty()) { return {m_num_channels, m_max_block_size}; }
    thl::core::Buffer<float> buffer = std::move(m_buffer_pool.back());
    m_buffer_pool.pop_back();
    for (size_t ch = 0; ch < buffer.get_num_channels(); ++ch) {
        std::fill_n(buffer.get_write_pointer(ch), buffer.get_num_samples(), 0.0f);
    }
    return buffer;
}

void Graph::collect_garbage() {
    // Nested graphs keep their own versions; one timer on the outer graph serves all.
    std::vector<Graph*> graphs{this};
    while (!graphs.empty()) {
        Graph* graph = graphs.back();
        graphs.pop_back();
        graph->collect_own_garbage();
        for (auto& [id, node] : graph->m_nodes) {
            if (auto* nested = dynamic_cast<Graph*>(node->m_processor.get())) {
                graphs.push_back(nested);
            }
        }
    }
}

void Graph::collect_own_garbage() {
    CompiledGraph* returned = nullptr;
    while (m_returned.try_pop(returned)) {
        m_live_generations.erase(returned->m_generation);
        recycle(returned);
    }

    const uint64_t oldest_live = m_live_generations.empty() ? std::numeric_limits<uint64_t>::max()
                                                            : *m_live_generations.begin();
    std::erase_if(m_retired_nodes, [oldest_live](const RetiredNode& retired) {
        return retired.m_last_generation < oldest_live;
    });
    std::erase_if(m_retired_delays, [oldest_live](const RetiredDelay& retired) {
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
    m_retired_delays.clear();
}

// ─────────────────────────────────────────────────────────────────────────────
// Processing
// ─────────────────────────────────────────────────────────────────────────────

bool Graph::switch_to_pending() {
    if (m_pending.load(std::memory_order_acquire) == nullptr) { return false; }

    // If the control thread has not emptied m_returned yet, switch at a later block instead.
    if (m_active != nullptr && !m_returned.try_push(m_active)) { return false; }
    m_active = m_pending.exchange(nullptr, std::memory_order_acq_rel);
    return true;
}

uint32_t Graph::apply_latencies(CompiledGraph& compiled) {
    auto& path_latencies = compiled.m_path_latencies;

    // Delays each source to the slowest source of its node; returns that path latency.
    const auto align = [&](std::span<Source> sources) {
        uint32_t path_latency = 0;
        for (const Source& source : sources) {
            path_latency = std::max(path_latency, path_latencies[source.m_buffer]);
        }
        for (Source& source : sources) {
            source.m_delay.set_delay_samples(path_latency - path_latencies[source.m_buffer]);
        }
        return path_latency;
    };

    for (ProcessingStep& step : compiled.m_steps) {
        const uint32_t latency = step.m_node->m_processor->latency_samples();
        step.m_dry_delay->set_delay_samples(latency);
        path_latencies[step.m_buffer] = align(step.m_sources) + latency;
    }
    return align(compiled.m_output_sources);
}

void Graph::apply_fade(thl::core::BufferView buffer) {
    const size_t n = buffer.get_num_samples();
    const bool pending = m_pending.load(std::memory_order_acquire) != nullptr;
    if (n == 0 || (!pending && m_fade.get_smoothed_value(0) == 1.0f)) { return; }

    size_t fade_out_start = n;
    if (pending && !m_fading_out) {
        fade_out_start = n >= m_fade_samples ? n - m_fade_samples : 0;
        m_fading_out = true;
    }

    for (size_t i = 0; i < n; ++i) {
        if (i == fade_out_start) { m_fade.set_target_value(0.0f); }
        const float gain = m_fade.get_smoothed_value();
        for (size_t ch = 0; ch < buffer.get_num_channels(); ++ch) {
            buffer.get_write_pointer(ch)[i] *= gain;
        }
    }

    if (!m_fading_out || m_fade.get_smoothed_value(0) != 0.0f) { return; }

    const bool switched = switch_to_pending();
    if (switched) {
        m_fading_out = false;
        m_fade.set_target_value(1.0f);
    }
}

void Graph::mix_sources(const CompiledGraph& compiled,
                        std::span<Source> sources,
                        Node& node,
                        thl::core::BufferView destination,
                        thl::core::BufferView dry) {
    const size_t n = destination.get_num_samples();
    auto& smoother = node.m_input_mix_smoother;
    const bool average = node.m_input_mix.load(std::memory_order_relaxed) == InputMix::Average;
    const float target = average ? 1.0f : 0.0f;
    smoother.set_target_value(target);

    if (sources.empty()) {
        for (size_t ch = 0; ch < destination.get_num_channels(); ++ch) {
            std::fill_n(destination.get_write_pointer(ch), n, 0.0f);
        }
        smoother.get_smoothed_value(n);
        return;
    }

    for (size_t s = 0; s < sources.size(); ++s) {
        Source& source = sources[s];
        const auto& buffer = compiled.m_buffers[source.m_buffer];
        const bool delayed = source.m_delay.max_delay_samples() > 0;
        if (delayed) {
            for (size_t ch = 0; ch < dry.get_num_channels(); ++ch) {
                std::copy_n(buffer.get_read_pointer(ch), n, dry.get_write_pointer(ch));
            }
            source.m_delay.process(dry);
        }

        for (size_t ch = 0; ch < destination.get_num_channels(); ++ch) {
            const float* in = delayed ? dry.get_read_pointer(ch) : buffer.get_read_pointer(ch);
            float* out = destination.get_write_pointer(ch);
            if (s == 0) {
                std::copy_n(in, n, out);
            } else {
                for (size_t i = 0; i < n; ++i) { out[i] += in[i]; }
            }
        }
    }

    const float average_scale = 1.0f / static_cast<float>(sources.size());
    if (smoother.get_smoothed_value(0) == target) {
        if (!average) { return; }
        for (size_t ch = 0; ch < destination.get_num_channels(); ++ch) {
            float* out = destination.get_write_pointer(ch);
            for (size_t i = 0; i < n; ++i) { out[i] *= average_scale; }
        }
        return;
    }

    for (size_t i = 0; i < n; ++i) {
        const float amount = smoother.get_smoothed_value();
        const float scale = (1.0f - amount) + average_scale * amount;
        for (size_t ch = 0; ch < destination.get_num_channels(); ++ch) {
            destination.get_write_pointer(ch)[i] *= scale;
        }
    }
}

void Graph::run_step(const ProcessingStep& step,
                     thl::core::BufferView buffer,
                     thl::core::BufferView dry,
                     uint32_t modulation_offset) {
    Node& node = *step.m_node;
    thl::dsp::utils::DelayProcessor& dry_delay = *step.m_dry_delay;
    auto& smoother = node.m_wet_mix_smoother;
    const float target = node.m_wet_mix.load(std::memory_order_relaxed);
    smoother.set_target_value(target);

    if (smoother.get_smoothed_value(0) == target) {
        if (target == 0.0f) {  // bypassed: the sources, delayed by the node's latency
            dry_delay.process(buffer);
            return;
        }
        if (target == 1.0f) {
            dry_delay.write(buffer);
            node.m_processor->process_modulated(buffer, modulation_offset);
            return;
        }
    }

    const size_t n = buffer.get_num_samples();
    for (size_t ch = 0; ch < buffer.get_num_channels(); ++ch) {
        std::copy_n(buffer.get_read_pointer(ch), n, dry.get_write_pointer(ch));
    }
    dry_delay.process(dry);
    node.m_processor->process_modulated(buffer, modulation_offset);

    for (size_t i = 0; i < n; ++i) {
        const float wet = smoother.get_smoothed_value();
        for (size_t ch = 0; ch < buffer.get_num_channels(); ++ch) {
            const float d = dry.get_read_pointer(ch)[i];
            float& out = buffer.get_write_pointer(ch)[i];
            out = d * (1.0f - wet) + out * wet;
        }
    }
}

void Graph::process(thl::core::BufferView buffer, uint32_t modulation_offset) {
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

    set_latency_samples(apply_latencies(*compiled));

    if (compiled->m_input_buffer) {
        auto& input = compiled->m_buffers[*compiled->m_input_buffer];
        for (size_t ch = 0; ch < compiled->m_num_channels; ++ch) {
            std::copy_n(buffer.get_read_pointer(ch), n, input.get_write_pointer(ch));
        }
    }

    const thl::core::BufferView dry(compiled->m_dry.get_array_of_write_pointers(),
                                    compiled->m_num_channels,
                                    n);
    for (ProcessingStep& step : compiled->m_steps) {
        auto& own = compiled->m_buffers[step.m_buffer];
        const thl::core::BufferView node_buffer(own.get_array_of_write_pointers(),
                                                compiled->m_num_channels,
                                                n);
        mix_sources(*compiled, step.m_sources, *step.m_node, node_buffer, dry);
        run_step(step, node_buffer, dry, modulation_offset);
    }

    mix_sources(*compiled, compiled->m_output_sources, *compiled->m_output_node, buffer, dry);
    apply_fade(buffer);
}

}  // namespace thl::graph
