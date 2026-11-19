#include <tanh/core/AbiNamespace.h>
#include <tanh/core/Buffer.h>
#include <tanh/core/BufferOperations.h>
#include <tanh/core/BufferView.h>
#include <tanh/dsp/BaseProcessor.h>
#include <tanh/dsp/probe/Probe.h>
#include <tanh/dsp/utils/DelayProcessor.h>
#include <tanh/dsp/utils/Mixer.h>
#include <tanh/dsp/utils/SmoothedValue.h>
#include <tanh/graph/Graph.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <ranges>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace thl::inline THL_ABI_NAMESPACE::graph {

// ─────────────────────────────────────────────────────────────────────────────
// Node: what the graph keeps per node, across versions. graph_input() and
// graph_output() have no processor.
// ─────────────────────────────────────────────────────────────────────────────

struct Graph::Node {
    static constexpr double k_ramp_seconds = 0.02;

    std::unique_ptr<thl::dsp::BaseProcessor> m_processor;

    // Live settings: written on the control thread, read by the audio thread every block.
    std::atomic<InputMix> m_input_mix{InputMix::Sum};
    std::atomic<float> m_wet_mix{1.0f};
    std::atomic<MixLaw> m_mix_law{MixLaw::Linear};

    // Sized by compile() for the latest version. A replaced one is retired, not deleted: a version
    // still running uses it through its own pointer (ProcessingStep, Inputs).
    std::unique_ptr<thl::dsp::utils::Mixer> m_input_mixer;         // one input per source
    std::unique_ptr<thl::dsp::utils::DelayProcessor> m_dry_delay;  // the processor's max latency

    // Audio thread: input 0 dry, input 1 wet. Never replaced, so a glide carries over commits.
    thl::dsp::utils::Mixer m_output_mixer;
    // Audio thread: where the dry signal comes from, 1 the processor (phase-matched), 0 the
    // sources: the processor while the node runs, the sources when it is bypassed, gliding
    // between the two with the wet mix so the bypass continues what the blend ended on. Never
    // replaced.
    thl::dsp::utils::SmoothedValue m_dry_source;

    // Probes watching the node, per ProbePoint: set on the control thread, read by the audio
    // thread (seq_cst against Graph::m_audio_epoch, see remove_probe()).
    struct Probes {
        std::array<std::atomic<thl::dsp::probe::ProbeBase*>, k_max_probes_per_point> m_slots{};
        std::atomic<uint32_t> m_count{0};  // lets a node without probes skip the slots
    };
    std::array<Probes, 2> m_probes;  // ProbePoint::Input, ProbePoint::Output
};

// ─────────────────────────────────────────────────────────────────────────────
// The compiled version of the graph. Built on the control thread, then owned by
// the audio thread until it hands it back through m_returned.
// ─────────────────────────────────────────────────────────────────────────────

struct Graph::Source {
    BufferIndex m_buffer = 0;
    thl::dsp::utils::DelayProcessor m_delay;  // to the slowest source of the node
    thl::core::Buffer<float> m_delayed;       // allocated only if m_delay can delay
};

/// A node's sources, each delayed to the slowest, mixed by the node's input Mixer.
struct Graph::Inputs {
    std::vector<Source> m_sources;
    std::vector<thl::core::ConstBufferView> m_views;  // one per source, filled every block
    thl::dsp::utils::Mixer* m_mixer = nullptr;        // the one this version was compiled with

    /// Audio thread: mixes the sources (from @p buffers) into @p destination, summed or averaged.
    void mix(std::span<thl::core::Buffer<float>> buffers,
             InputMix input_mix,
             const thl::core::BufferView& destination) TANH_NONBLOCKING_FUNCTION;
};

/// One node as this version runs it.
struct Graph::ProcessingStep {
    Node* m_node = nullptr;
    Inputs m_inputs;
    BufferIndex m_buffer = 0;
    // The dry delay this version was compiled with.
    thl::dsp::utils::DelayProcessor* m_dry_delay = nullptr;
    uint32_t m_max_latency = 0;    // the node's max_latency_samples() when compiled: what the
                                   // delays were sized for
    uint32_t m_input_latency = 0;  // set by apply_latencies(): the sources' path latency

    /// Audio thread: runs the processor on @p buffer (its mixed inputs) and blends the result with
    /// its dry signal by the node's wet mix and mix law. @p dry and @p sources are scratch.
    void process(const thl::core::BufferView& buffer,
                 const thl::core::BufferView& dry,
                 const thl::core::BufferView& sources,
                 uint32_t modulation_offset) TANH_NONBLOCKING_FUNCTION;
};

struct Graph::CompiledGraph {
    uint64_t m_generation = 0;
    std::vector<ProcessingStep> m_steps;  // in processing order
    std::vector<thl::core::Buffer<float>> m_buffers;
    std::vector<uint32_t> m_path_latencies;  // per buffer, set by apply_latencies()
    uint32_t m_max_latency = 0;              // of the output
    uint32_t m_expected_latency = 0;     // of the output, with the nodes' latencies when compiled
    thl::core::Buffer<float> m_dry;      // scratch for a step's dry signal
    thl::core::Buffer<float> m_sources;  // scratch for a step's input while its dry signal glides
    std::optional<BufferIndex> m_input_buffer;  // empty if graph_input() reaches no output
    Inputs m_output_inputs;
    Node* m_output_node = nullptr;
    size_t m_num_channels = 0;
    size_t m_max_block_size = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// Mix laws
// ─────────────────────────────────────────────────────────────────────────────

namespace {

using thl::dsp::utils::Mixer;

/// The input Mixer's mode for @p input_mix: Average normalises the weights (all 1) to 1/N.
Mixer::Mode input_mode(Graph::InputMix input_mix) {
    return input_mix == Graph::InputMix::Average ? Mixer::Mode::Mix : Mixer::Mode::Sum;
}

/// Sets @p mixer (input 0 dry, input 1 wet) to @p law at @p wet_mix (clamped to [0, 1]); it glides
/// there. Every law but Parallel normalises its gains (Mix) at the p its curve keeps, so a glide
/// stays on the curve; Parallel's curve is a straight line, so its gains glide as they are (Sum).
void apply_mix_law(Mixer& mixer, Graph::MixLaw law, float wet_mix) TANH_NONBLOCKING_FUNCTION {
    const float m = std::clamp(wet_mix, 0.0f, 1.0f);
    const double quarter = 0.5 * std::numbers::pi * m;
    Mixer::Mode mode = Mixer::Mode::Mix;
    float dry = 1.0f - m;
    float wet = m;
    switch (law) {
        case Graph::MixLaw::Linear: mixer.set_p(Mixer::k_amplitude); break;
        case Graph::MixLaw::ConstantPower:
            mixer.set_p(Mixer::k_power);
            dry = static_cast<float>(std::cos(quarter));
            wet = static_cast<float>(std::sin(quarter));
            break;
        case Graph::MixLaw::Compromise:  // the geometric mean of the two above: -4.5 dB at 0.5
            mixer.set_p(Mixer::k_compromise);
            dry = static_cast<float>(std::sqrt((1.0 - m) * std::cos(quarter)));
            wet = static_cast<float>(std::sqrt(m * std::sin(quarter)));
            break;
        case Graph::MixLaw::Parallel:
            mode = Mixer::Mode::Sum;
            dry = 1.0f;
            break;
        case Graph::MixLaw::DryHeld:  // one of the two is always 1: the peak norm keeps them
            mixer.set_p(Mixer::k_peak);
            dry = std::min(1.0f, 2.0f * (1.0f - m));
            wet = std::min(1.0f, 2.0f * m);
            break;
    }
    // Exact at the ends (cos(pi/2) is not exactly 0), so a fully dry or fully wet node takes the
    // fast paths.
    if (m == 0.0f) {
        dry = 1.0f;
        wet = 0.0f;
    } else if (m == 1.0f && law != Graph::MixLaw::Parallel) {
        dry = 0.0f;
        wet = 1.0f;
    }
    mixer.set_mode(mode);
    mixer.set_weights(std::array{dry, wet});
}

/// Where a node's dry signal comes from (Node::m_dry_source): the processor while @p wet_mix runs
/// it, the sources at 0, where the node is bypassed.
float dry_source_of(float wet_mix) TANH_NONBLOCKING_FUNCTION {
    return wet_mix > 0.0f ? 1.0f : 0.0f;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

Graph::Graph() {
    m_output_node = m_nodes.emplace(k_output_id, std::make_unique<Node>()).first->second.get();
    m_input_node = m_nodes.emplace(k_input_id, std::make_unique<Node>()).first->second.get();
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

    // Its probes stop now: every slot cleared, one wait, then they go.
    std::vector<std::unique_ptr<thl::dsp::probe::ProbeBase>> detached;
    for (auto entry = m_probes.begin(); entry != m_probes.end();) {
        const ProbeId probe_id = entry->first;
        const bool on_node = entry->second.m_node == id;
        ++entry;
        if (on_node) { detached.push_back(detach_probe(probe_id)); }
    }
    if (!detached.empty()) { wait_for_audio_thread(); }
    detached.clear();

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

bool Graph::set_mix_law(NodeId id, MixLaw law) {
    if (id == k_input_id || id == k_output_id) { return false; }
    const auto it = m_nodes.find(id);
    if (it == m_nodes.end()) { return false; }
    it->second->m_mix_law.store(law, std::memory_order_relaxed);
    return true;
}

std::optional<Graph::MixLaw> Graph::mix_law(NodeId id) const {
    if (id == k_input_id || id == k_output_id) { return std::nullopt; }
    const auto it = m_nodes.find(id);
    if (it == m_nodes.end()) { return std::nullopt; }
    return it->second->m_mix_law.load(std::memory_order_relaxed);
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
    for (auto& [id, node] : m_nodes) { prepare_node(*node); }
    for (auto& [id, entry] : m_probes) {
        entry.m_probe->prepare(m_sample_rate, m_max_block_size, m_num_channels);
    }
    set_max_latency_samples(0);
    rebuild();

    if (switch_to_pending()) { set_latency_samples(apply_latencies(*m_active)); }
    m_restart_probes = false;  // nothing ran yet: the probes were just prepared, no gap to mark
    m_fade.reset(m_sample_rate, k_fade_seconds);
    m_fade.set_current_and_target_value(1.0f);
    m_fade_samples = static_cast<size_t>(k_fade_seconds * m_sample_rate) + 1;
    m_fading_out = false;
    m_hold_samples = 0;
}

void Graph::prepare_node(Node& node, bool prepare_processor) const {
    if (prepare_processor && node.m_processor) {
        node.m_processor->prepare(m_sample_rate, m_max_block_size, m_num_channels);
    }
    if (node.m_dry_delay) {
        node.m_dry_delay->prepare(m_sample_rate, m_max_block_size, m_num_channels);
    }
    // Start at the current settings instead of gliding to them.
    if (node.m_input_mixer) {
        Mixer& input_mixer = *node.m_input_mixer;
        input_mixer.prepare(m_sample_rate, input_mixer.num_inputs(), Node::k_ramp_seconds);
        input_mixer.set_mode(input_mode(node.m_input_mix.load(std::memory_order_relaxed)));
        input_mixer.reset();
    }
    node.m_output_mixer.prepare(m_sample_rate, 2, Node::k_ramp_seconds);
    apply_mix_law(node.m_output_mixer,
                  node.m_mix_law.load(std::memory_order_relaxed),
                  node.m_wet_mix.load(std::memory_order_relaxed));
    node.m_dry_source.reset(m_sample_rate, Node::k_ramp_seconds);
    node.m_dry_source.set_current_and_target_value(
        dry_source_of(node.m_wet_mix.load(std::memory_order_relaxed)));
    node.m_output_mixer.reset();
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
        if (max_delay > 0) {
            source.m_delayed = thl::core::Buffer<float>(m_num_channels, m_max_block_size);
        }
    }
}

Graph::Inputs Graph::make_inputs(Node& node, std::vector<Source> sources) {
    const size_t num_sources = sources.size();
    const bool keep_mixer = node.m_input_mixer && node.m_input_mixer->num_inputs() == num_sources;
    if (!keep_mixer) {
        if (node.m_input_mixer) {
            m_retired_mixers.push_back({.m_mixer = std::move(node.m_input_mixer),
                                        .m_last_generation = m_next_generation - 1});
        }
        node.m_input_mixer = std::make_unique<Mixer>();
        // Before prepare() there is no sample rate; prepare() compiles again.
        if (m_max_block_size > 0) {
            node.m_input_mixer->prepare(m_sample_rate, num_sources, Node::k_ramp_seconds);
        }
        node.m_input_mixer->set_mode(input_mode(node.m_input_mix.load(std::memory_order_relaxed)));
        node.m_input_mixer->reset();
    }
    return {.m_sources = std::move(sources),
            .m_views = std::vector<thl::core::ConstBufferView>(num_sources),
            .m_mixer = node.m_input_mixer.get()};
}

bool Graph::max_latencies_changed() const {
    for (const auto& [id, node] : m_nodes) {
        if (node->m_dry_delay &&
            node->m_dry_delay->max_delay_samples() != node->m_processor->max_latency_samples()) {
            return true;
        }
    }
    return false;
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
    compiled->m_dry = thl::core::Buffer<float>(m_num_channels, m_max_block_size);
    compiled->m_sources = thl::core::Buffer<float>(m_num_channels, m_max_block_size);

    // `order` has every node after its sources, so a source's buffer always
    // exists by the time a node looks it up.
    std::map<NodeId, BufferIndex> buffer_of;
    std::vector<uint32_t> max_path_latencies;       // per buffer
    std::vector<uint32_t> expected_path_latencies;  // per buffer, with the current latencies
    for (const NodeId id : *order) {
        Node& node = *m_nodes.at(id);
        std::vector<Source> sources;
        uint32_t max_path_latency = 0;
        uint32_t expected_path_latency = 0;
        for (const NodeId source : sources_of(id)) {
            const BufferIndex buffer = buffer_of.at(source);
            sources.push_back({.m_buffer = buffer});
            max_path_latency = std::max(max_path_latency, max_path_latencies[buffer]);
            expected_path_latency =
                std::max(expected_path_latency, expected_path_latencies[buffer]);
        }
        prepare_delays(sources, max_path_latencies);

        if (id == k_output_id) {
            compiled->m_output_inputs = make_inputs(node, std::move(sources));
            compiled->m_output_node = &node;
            compiled->m_max_latency = max_path_latency;
            compiled->m_expected_latency = expected_path_latency;
            continue;
        }

        const uint32_t max = node.m_processor ? node.m_processor->max_latency_samples() : 0;
        // What the node will have when this version runs: a nested graph the version it just
        // committed (it commits first); a node whose maximum changed since the last commit its
        // maximum, since its current latency may still be that of what it ran before (e.g. it
        // wraps a graph of its own) until it runs again; any other node its current latency.
        uint32_t expected = 0;
        if (node.m_processor) {
            const auto* nested = dynamic_cast<const Graph*>(node.m_processor.get());
            const bool max_changed =
                node.m_dry_delay && node.m_dry_delay->max_delay_samples() != max;
            if (nested != nullptr) {
                expected = std::min(nested->committed_latency_samples(), max);
            } else if (max_changed) {
                expected = max;
            } else {
                expected = std::min(node.m_processor->latency_samples(), max);
            }
        }
        const auto index = static_cast<BufferIndex>(compiled->m_buffers.size());
        compiled->m_buffers.emplace_back(m_num_channels, m_max_block_size);
        max_path_latencies.push_back(max_path_latency + max);
        expected_path_latencies.push_back(expected_path_latency + expected);
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
                                     .m_inputs = make_inputs(node, std::move(sources)),
                                     .m_buffer = index,
                                     .m_dry_delay = node.m_dry_delay.get(),
                                     .m_max_latency = max});
    }
    compiled->m_path_latencies.assign(compiled->m_buffers.size(), 0);
    return compiled;
}

template <typename Self>
std::vector<Self*> Graph::graph_tree(Self& root) {
    std::vector<Self*> graphs{&root};
    for (size_t i = 0; i < graphs.size(); ++i) {
        for (const auto& [id, node] : graphs[i]->m_nodes) {
            if (auto* nested = dynamic_cast<Self*>(node->m_processor.get())) {
                graphs.push_back(nested);
            }
        }
    }
    return graphs;
}

bool Graph::commit() {
    const auto graphs = graph_tree(*this);
    bool committed = true;
    // Nested graphs first, so their new max latency is set before the graph containing them checks
    // it.
    for (Graph* graph : std::ranges::reverse_view(graphs)) {
        committed = graph->commit_own() && committed;
    }
    return committed;
}

bool Graph::commit_own() {
    if (!m_dirty && m_published && !max_latencies_changed()) {  // nothing to hand over: no fade
        collect_own_garbage();
        return true;
    }
    return rebuild();
}

bool Graph::has_uncommitted_changes() const {
    for (const Graph* graph : graph_tree(*this)) {
        if (graph->m_dirty || graph->max_latencies_changed()) { return true; }
    }
    return false;
}

bool Graph::rebuild() {
    collect_garbage();
    auto compiled = compile();
    if (!compiled) { return false; }

    compiled->m_generation = m_next_generation++;
    if (compiled->m_max_latency > max_latency_samples()) {
        set_max_latency_samples(compiled->m_max_latency);
    }
    m_committed_latency = compiled->m_expected_latency;
    publish(std::move(compiled));
    m_dirty = false;
    return true;
}

void Graph::publish(std::unique_ptr<CompiledGraph> compiled) {
    m_live_generations.insert(compiled->m_generation);
    m_published = true;
    const CompiledGraph* stale = m_pending.exchange(compiled.release(), std::memory_order_acq_rel);

    // The audio thread never picked this one up, so it can be freed right away.
    if (stale != nullptr) {
        m_live_generations.erase(stale->m_generation);
        delete stale;
    }
}

void Graph::collect_garbage() {
    // Nested graphs keep their own versions; one timer on the outer graph serves all.
    for (Graph* graph : graph_tree(*this)) { graph->collect_own_garbage(); }
}

void Graph::collect_own_garbage() {
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
    std::erase_if(m_retired_delays, [oldest_live](const RetiredDelay& retired) {
        return retired.m_last_generation < oldest_live;
    });
    std::erase_if(m_retired_mixers, [oldest_live](const RetiredMixer& retired) {
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
    m_retired_mixers.clear();
}

// ─────────────────────────────────────────────────────────────────────────────
// Processing
// ─────────────────────────────────────────────────────────────────────────────

bool Graph::switch_to_pending() {
    if (m_pending.load(std::memory_order_acquire) == nullptr) { return false; }

    // The running version's probes start over now, before it is handed back (the control thread
    // may free it then): a node that no longer runs keeps nothing of before.
    if (m_active != nullptr) { reset_step_probes(*m_active); }
    // If the control thread has not emptied m_returned yet, switch at a later block instead.
    if (m_active != nullptr && !m_returned.try_push(m_active)) { return false; }
    m_active = m_pending.exchange(nullptr, std::memory_order_acq_rel);
    // The new version starts after a fade and a silence (or a node comes back that did not run):
    // its probes and the output's start over at the next block, after the output's probes got
    // this one, the old version's last. What process() is given has no gap: graph_input()'s
    // probes go on.
    m_restart_probes = true;
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
        // A node whose latency rose above the maximum this version was compiled with (it raised
        // its max_latency_samples(), or a nested graph committed on its own) is aligned at that
        // maximum until the next commit: the delays were sized for it.
        const uint32_t latency =
            std::min(step.m_node->m_processor->latency_samples(), step.m_max_latency);
        step.m_dry_delay->set_delay_samples(latency);
        step.m_input_latency = align(step.m_inputs.m_sources);
        path_latencies[step.m_buffer] = step.m_input_latency + latency;
    }
    return align(compiled.m_output_inputs.m_sources);
}

void Graph::apply_fade(thl::core::BufferView buffer) {
    const size_t n = buffer.get_num_samples();
    const bool pending = m_pending.load(std::memory_order_acquire) != nullptr;
    if (n == 0 || (!pending && m_fade.get_smoothed_value(0) == 1.0f)) { return; }

    size_t fade_out_start = n;
    if (pending && !m_fading_out) {
        fade_out_start = n >= m_fade_samples ? n - m_fade_samples : 0;
        m_fading_out = true;
        m_hold_samples = 0;
    }

    for (size_t i = 0; i < n; ++i) {
        if (i == fade_out_start) { m_fade.set_target_value(0.0f); }
        if (m_hold_samples > 0) {
            --m_hold_samples;
            if (m_hold_samples == 0) { m_fade.set_target_value(1.0f); }
        }
        const float gain = m_fade.get_smoothed_value();
        for (size_t ch = 0; ch < buffer.get_num_channels(); ++ch) {
            buffer.get_write_pointer(ch)[i] *= gain;
        }
    }

    if (!m_fading_out || m_fade.get_smoothed_value(0) != 0.0f) { return; }

    const bool switched = switch_to_pending();
    if (switched) {
        m_fading_out = false;
        m_hold_samples = apply_latencies(*m_active);
        set_latency_samples(m_hold_samples);
        if (m_hold_samples == 0) { m_fade.set_target_value(1.0f); }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Probes
// ─────────────────────────────────────────────────────────────────────────────

namespace {
constexpr size_t index_of(Graph::ProbePoint point) {
    return point == Graph::ProbePoint::Input ? 0 : 1;
}

/// While alive, the audio thread is inside process() or reset(): m_audio_epoch is odd.
class AudioEpoch {
public:
    explicit AudioEpoch(std::atomic<uint64_t>& epoch) TANH_NONBLOCKING_FUNCTION : m_epoch(epoch) {
        m_epoch.fetch_add(1, std::memory_order_seq_cst);
    }
    ~AudioEpoch() TANH_NONBLOCKING_FUNCTION { m_epoch.fetch_add(1, std::memory_order_seq_cst); }
    AudioEpoch(const AudioEpoch&) = delete;
    AudioEpoch& operator=(const AudioEpoch&) = delete;

private:
    std::atomic<uint64_t>& m_epoch;
};
}  // namespace

std::optional<ProbeId> Graph::add_probe(NodeId id,
                                        ProbePoint point,
                                        std::unique_ptr<thl::dsp::probe::ProbeBase> probe) {
    if (probe == nullptr) { return std::nullopt; }
    const auto it = m_nodes.find(id);
    if (it == m_nodes.end()) { return std::nullopt; }
    if ((id == k_input_id && point != ProbePoint::Output) ||
        (id == k_output_id && point != ProbePoint::Input)) {
        return std::nullopt;
    }
    Node::Probes& probes = it->second->m_probes[index_of(point)];
    for (size_t slot = 0; slot < k_max_probes_per_point; ++slot) {
        if (probes.m_slots[slot].load(std::memory_order_relaxed) != nullptr) { continue; }
        // Ready, and owned by the graph, before the audio thread can see it: if the entry cannot
        // be added, the probe goes without ever having been published.
        if (m_max_block_size > 0) {
            probe->prepare(m_sample_rate, m_max_block_size, m_num_channels);
        }
        const ProbeId probe_id{m_next_probe_id++};
        thl::dsp::probe::ProbeBase* const published = probe.get();
        m_probes.emplace(probe_id,
                         ProbeEntry{.m_node = id,
                                    .m_point = point,
                                    .m_slot = slot,
                                    .m_probe = std::move(probe)});
        probes.m_slots[slot].store(published, std::memory_order_seq_cst);
        probes.m_count.fetch_add(1, std::memory_order_seq_cst);
        return probe_id;
    }
    return std::nullopt;
}

std::unique_ptr<thl::dsp::probe::ProbeBase> Graph::detach_probe(ProbeId id) {
    const auto it = m_probes.find(id);
    if (it == m_probes.end()) { return nullptr; }
    ProbeEntry entry = std::move(it->second);
    m_probes.erase(it);
    Node::Probes& probes = m_nodes.at(entry.m_node)->m_probes[index_of(entry.m_point)];
    probes.m_slots[entry.m_slot].store(nullptr, std::memory_order_seq_cst);
    probes.m_count.fetch_sub(1, std::memory_order_seq_cst);
    return std::move(entry.m_probe);
}

bool Graph::remove_probe(ProbeId id) {
    const std::unique_ptr<thl::dsp::probe::ProbeBase> probe = detach_probe(id);
    if (probe == nullptr) { return false; }
    wait_for_audio_thread();
    return true;  // deleted here, after the audio thread let go of it
}

thl::dsp::probe::ProbeBase* Graph::probe(ProbeId id) const {
    const auto it = m_probes.find(id);
    return it == m_probes.end() ? nullptr : it->second.m_probe.get();
}

void Graph::wait_for_audio_thread() const {
    // seq_cst with the audio thread's epoch increment and slot loads: either the audio thread's
    // next load of the slot sees nullptr, or this load sees it inside a block (odd), which the
    // wait lets end. Audio stopped (even): nothing to wait for.
    const uint64_t epoch = m_audio_epoch.load(std::memory_order_seq_cst);
    if ((epoch & 1U) == 0) { return; }
    [[maybe_unused]] const auto start = std::chrono::steady_clock::now();
    while (m_audio_epoch.load(std::memory_order_seq_cst) == epoch) {
        // One block never takes seconds: the caller is most likely the audio thread itself,
        // inside process(), which would wait for itself forever.
        assert(std::chrono::steady_clock::now() - start < std::chrono::seconds(2) &&
               "Graph: remove_probe()/remove_node() called from the audio thread?");
        std::this_thread::yield();
    }
}

void Graph::run_probes(Node& node,
                       ProbePoint point,
                       const thl::core::BufferView& block,
                       uint32_t latency_samples) {
    const Node::Probes& probes = node.m_probes[index_of(point)];
    const uint32_t count = probes.m_count.load(std::memory_order_relaxed);
    uint32_t found = 0;
    for (const auto& slot : probes.m_slots) {
        if (found == count) { break; }  // the rest are empty
        thl::dsp::probe::ProbeBase* probe = slot.load(std::memory_order_seq_cst);
        if (probe == nullptr) { continue; }
        ++found;
        probe->process(block, latency_samples);
    }
}

void Graph::reset_probes(Node& node) {
    for (const Node::Probes& probes : node.m_probes) {
        const uint32_t count = probes.m_count.load(std::memory_order_relaxed);
        uint32_t found = 0;
        for (const auto& slot : probes.m_slots) {
            if (found == count) { break; }
            thl::dsp::probe::ProbeBase* probe = slot.load(std::memory_order_seq_cst);
            if (probe == nullptr) { continue; }
            ++found;
            probe->reset();
        }
    }
}

void Graph::reset_step_probes(const CompiledGraph& compiled) {
    for (const ProcessingStep& step : compiled.m_steps) { reset_probes(*step.m_node); }
}

void Graph::reset_all_probes() {
    reset_probes(*m_input_node);
    reset_probes(*m_output_node);
    if (m_active != nullptr) { reset_step_probes(*m_active); }
}

void Graph::Inputs::mix(std::span<thl::core::Buffer<float>> buffers,
                        InputMix input_mix,
                        const thl::core::BufferView& destination) {
    const size_t num_channels = destination.get_num_channels();
    const size_t num_samples = destination.get_num_samples();
    for (size_t s = 0; s < m_sources.size(); ++s) {
        Source& source = m_sources[s];
        const thl::core::ConstBufferView buffer(
            buffers[source.m_buffer].get_array_of_read_pointers(),
            num_channels,
            num_samples);
        if (source.m_delay.max_delay_samples() == 0) {
            m_views[s] = buffer;
            continue;
        }
        const thl::core::BufferView delayed(source.m_delayed.get_array_of_write_pointers(),
                                            num_channels,
                                            num_samples);
        thl::core::BufferOperations::copy(delayed, buffer);
        source.m_delay.process(delayed);
        m_views[s] = delayed;
    }
    m_mixer->set_mode(input_mode(input_mix));
    m_mixer->process(m_views, destination);
}

void Graph::ProcessingStep::process(const thl::core::BufferView& buffer,
                                    const thl::core::BufferView& dry,
                                    const thl::core::BufferView& sources,
                                    uint32_t modulation_offset) {
    Node& node = *m_node;
    Mixer& mixer = node.m_output_mixer;
    thl::dsp::utils::SmoothedValue& dry_source = node.m_dry_source;
    // Settled at 0 or 1.
    const auto dry_source_is = [&dry_source](float value) {
        return !dry_source.is_smoothing() && dry_source.get_smoothed_value(0) == value;
    };
    const auto bypassed = [&] { return mixer.passes_only(0) && dry_source_is(0.0f); };
    const bool was_bypassed = bypassed();
    const float wet_mix = node.m_wet_mix.load(std::memory_order_relaxed);
    apply_mix_law(mixer, node.m_mix_law.load(std::memory_order_relaxed), wet_mix);
    dry_source.set_target_value(dry_source_of(wet_mix));

    if (bypassed()) {
        m_dry_delay->process(buffer);  // bypassed: the sources, delayed by the node's latency
        return;
    }
    // Back from bypass: the processor starts clean instead of where it froze.
    if (was_bypassed) { node.m_processor->reset(); }

    // Whenever the processor runs, its dry signal comes from it (process_with_dry()): the input
    // with the processor's phase response but without its effect, so a blend of a node that
    // shifts the phase (a crossover) does not comb. Towards and back from bypass it glides from
    // and to the sources themselves, which the bypass passes, so that no switch steps. The dry
    // delay then aligns it to the node's latency, which the dry signal never includes; it takes
    // the glide before the delay, so its history is continuous too. Fully wet, the dry signal
    // only keeps the delay's history current, so a blend starts from it.
    const bool own_dry = dry_source_is(1.0f);
    if (!own_dry) { thl::core::BufferOperations::copy(sources, buffer); }
    node.m_processor->process_modulated_with_dry(buffer, dry, modulation_offset);
    if (!own_dry) {
        thl::core::BufferView glide = dry;
        const size_t num_channels = glide.get_num_channels();
        for (size_t i = 0; i < glide.get_num_samples(); ++i) {
            const float own = dry_source.get_smoothed_value();
            for (size_t ch = 0; ch < num_channels; ++ch) {
                const float source = sources.get_read_pointer(ch)[i];
                float& sample = glide.get_write_pointer(ch)[i];
                sample = source + (own * (sample - source));  // exact where the two agree
            }
        }
    }
    if (mixer.passes_only(1)) {
        m_dry_delay->write(dry);
        return;
    }
    m_dry_delay->process(dry);
    mixer.process(std::array<thl::core::ConstBufferView, 2>{dry, buffer}, buffer);
}

void Graph::reset() {
    const AudioEpoch epoch(m_audio_epoch);
    reset_all_probes();
    CompiledGraph* compiled = m_active;
    if (compiled == nullptr) { return; }
    for (ProcessingStep& step : compiled->m_steps) {
        step.m_node->m_processor->reset();
        step.m_dry_delay->reset();
        for (Source& source : step.m_inputs.m_sources) { source.m_delay.reset(); }
    }
    for (Source& source : compiled->m_output_inputs.m_sources) { source.m_delay.reset(); }
}

void Graph::process(thl::core::BufferView buffer, uint32_t modulation_offset) {
    const AudioEpoch epoch(m_audio_epoch);
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
    if (m_restart_probes) {  // a version switch at the end of the last block
        m_restart_probes = false;
        reset_probes(*m_output_node);
        reset_step_probes(*compiled);
    }
    run_probes(*m_input_node, ProbePoint::Output, buffer, 0);

    if (compiled->m_input_buffer) {
        auto& input = compiled->m_buffers[*compiled->m_input_buffer];
        for (size_t ch = 0; ch < compiled->m_num_channels; ++ch) {
            std::copy_n(buffer.get_read_pointer(ch), n, input.get_write_pointer(ch));
        }
    }

    const thl::core::BufferView dry(compiled->m_dry.get_array_of_write_pointers(),
                                    compiled->m_num_channels,
                                    n);
    const thl::core::BufferView sources(compiled->m_sources.get_array_of_write_pointers(),
                                        compiled->m_num_channels,
                                        n);
    for (ProcessingStep& step : compiled->m_steps) {
        auto& own = compiled->m_buffers[step.m_buffer];
        const thl::core::BufferView node_buffer(own.get_array_of_write_pointers(),
                                                compiled->m_num_channels,
                                                n);
        step.m_inputs.mix(compiled->m_buffers,
                          step.m_node->m_input_mix.load(std::memory_order_relaxed),
                          node_buffer);
        run_probes(*step.m_node, ProbePoint::Input, node_buffer, step.m_input_latency);
        step.process(node_buffer, dry, sources, modulation_offset);
        run_probes(*step.m_node,
                   ProbePoint::Output,
                   node_buffer,
                   compiled->m_path_latencies[step.m_buffer]);
    }

    compiled->m_output_inputs.mix(
        compiled->m_buffers,
        compiled->m_output_node->m_input_mix.load(std::memory_order_relaxed),
        buffer);
    apply_fade(buffer);
    run_probes(*m_output_node, ProbePoint::Input, buffer, latency_samples());
}

}  // namespace thl::inline THL_ABI_NAMESPACE::graph
