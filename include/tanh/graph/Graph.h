#pragma once

#include <tanh/core/Buffer.h>
#include <tanh/core/BufferView.h>
#include <tanh/core/Exports.h>
#include <tanh/core/threading/LockFreeQueue.h>
#include <tanh/dsp/BaseProcessor.h>
#include <tanh/dsp/utils/DelayProcessor.h>
#include <tanh/dsp/utils/SmoothedValue.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <atomic>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace thl::graph {

/// A node of a Graph; never reused within a graph.
enum class NodeId : uint64_t {};

/// An edge of a Graph: @p m_from's output feeds @p m_to.
struct Connection {
    NodeId m_from{};
    NodeId m_to{};
    friend auto operator<=>(const Connection&, const Connection&) = default;
};

/// A graph of BaseProcessors that is itself a BaseProcessor. process() feeds the incoming buffer to
/// graph_input() and overwrites it with what reaches graph_output(). Every node runs with the
/// channel count given to prepare().
///
/// Edits are made on the control thread and take effect at commit(). The audio thread then fades
/// out the current version, switches to the committed one and, once its latency has passed, fades
/// it in. Every edit that changes the structure therefore goes silent for the fade and the
/// graph's latency, also where it does not touch the latent path. Input and wet mixes are live:
/// they ramp without a commit.
///
/// Latency compensation: a node's latency_samples() is followed per block; the faster sources of
/// every node and a node's dry path are delayed to match, up to the max_latency_samples() each node
/// had at the last commit.
class TANH_API Graph final : public thl::dsp::BaseProcessor {
public:
    /// How a node combines its sources: their sum (the default, as a mixer), or their sum
    /// divided by their number.
    enum class InputMix : uint8_t { Sum, Average };

    /// How a node's wet mix m (0 dry, 1 wet) sets the gains of its input (dry, delayed by its
    /// latency) and of its processor's output (wet).
    enum class MixLaw : uint8_t {
        /// dry 1 - m, wet m (the default): constant level when the wet signal is correlated with
        /// the dry (EQ, compression, saturation, phaser and comb effects).
        Linear,
        /// dry cos(m pi/2), wet sin(m pi/2): constant power when it is not (reverb, delay,
        /// chorus); -3 dB each at 0.5.
        ConstantPower,
        /// The geometric mean of Linear and ConstantPower, -4.5 dB each at 0.5: for a partly
        /// correlated wet signal (modulated delays, short rooms, multiband processing).
        Compromise,
        /// dry 1, wet m: parallel processing (parallel compression, sends); the mix only adds.
        Parallel,
        /// dry 1 up to 0.5 then down to 0, wet up to 1 by 0.5: full level in the middle, a send
        /// below it and an insert above.
        DryHeld,
    };

    Graph();
    ~Graph() override;

    Graph(const Graph&) = delete;
    Graph& operator=(const Graph&) = delete;
    Graph(Graph&&) = delete;
    Graph& operator=(Graph&&) = delete;

    // ─── Editing (control thread) ────────────
    /// A new node running a T made from @p args; see add_node(std::unique_ptr).
    template <typename T, typename... Args>
    std::optional<NodeId> add_node(Args&&... args) {
        static_assert(std::is_base_of_v<thl::dsp::BaseProcessor, T>);
        return add_node(std::make_unique<T>(std::forward<Args>(args)...));
    }

    /// A new node running @p processor, unconnected, until the next commit() not processed. A
    /// prepared graph prepares the processor here, on the calling thread. Nothing for a null
    /// processor.
    std::optional<NodeId> add_node(std::unique_ptr<thl::dsp::BaseProcessor> processor);

    /// A processor the caller already prepared, e.g. a heavy one on its own background thread,
    /// for @p sample_rate, @p samples_per_block and @p num_channels. The graph prepares it again
    /// only if they differ from its own, and always in prepare(), so before the graph is prepared
    /// this is add_node().
    std::optional<NodeId> add_prepared_node(std::unique_ptr<thl::dsp::BaseProcessor> processor,
                                            double sample_rate,
                                            size_t samples_per_block,
                                            size_t num_channels);

    /// Removes the node and its connections; its processor is freed once no version that ran it
    /// is left (collect_garbage()). False for graph_input(), graph_output() and unknown ids.
    bool remove_node(NodeId id);
    /// Feeds @p from's output into @p to. False for unknown ids, into graph_input(), out of
    /// graph_output(), a connection that exists or one that would close a cycle.
    bool connect(NodeId from, NodeId to);
    /// False if there is no such connection.
    bool disconnect(NodeId from, NodeId to);
    /// Live (no commit): how node @p id, or graph_output(), combines its sources; ramps over
    /// 20 ms. False for graph_input() and unknown ids.
    bool set_input_mix(NodeId id, InputMix input_mix);
    /// Live (no commit): 1 runs the node, 0 bypasses it (its sources pass, delayed by its
    /// latency), between them a blend; ramps over 20 ms, clamped to [0, 1]. A node coming back
    /// from 0 is reset() first. False for graph_input(), graph_output(), unknown ids and NaN.
    bool set_wet_mix(NodeId id, float wet_mix);
    /// Live (no commit): how node @p id's wet mix sets its dry and wet gains; a change crossfades
    /// from the old law over 20 ms. False for graph_input(), graph_output() and unknown ids.
    bool set_mix_law(NodeId id, MixLaw law);

    /// Hands the edited graph and every graph nested in it to the audio thread (a short fade out
    /// and in). Without changes since the last commit it does nothing; it also rebuilds when a
    /// node's max_latency_samples() changed. Commit the outermost graph: a nested graph's own
    /// commit() takes effect inside it, but if it raises its maximum latency, the graph containing
    /// it compensates at the old maximum until that graph commits (has_uncommitted_changes()).
    /// False if a graph has a cycle, which connect() prevents.
    bool commit();
    /// True after an edit, or when a node's max_latency_samples() changed, here or in a nested
    /// graph, since the last commit().
    bool has_uncommitted_changes() const;
    /// Control thread: the latency the version prepare() or commit() handed over last will have,
    /// from its nodes' current latency_samples() (a nested graph: its own committed latency; a
    /// node whose max_latency_samples() changed since the last commit: that maximum, until it
    /// runs and tells its own), so a host can be told right after commit(). The graph never pads:
    /// its output has the least latency its nodes allow, and max_latency_samples() only sizes the
    /// delays. A node that changes its latency between commits changes latency_samples() at the
    /// next block; tell the host then too (e.g. JUCE's setLatencySamples() from the message
    /// thread).
    uint32_t committed_latency_samples() const { return m_committed_latency; }

    /// Frees the versions the audio thread handed back and the removed nodes no running version
    /// uses, also in nested graphs. commit() calls it; call it from a timer (e.g. 30 Hz on the
    /// message thread) so removed processors go away without another edit. A nested graph that
    /// does not run (wet 0) keeps its old version until it runs again.
    void collect_garbage();

    /// The node that holds what process() is given.
    NodeId graph_input() const { return k_input_id; }
    /// The node whose sources make what process() returns.
    NodeId graph_output() const { return k_output_id; }

    /// The processor of node @p id; nullptr for graph_input(), graph_output() and unknown ids.
    thl::dsp::BaseProcessor* node(NodeId id);
    const thl::dsp::BaseProcessor* node(NodeId id) const;
    /// The wet mix set for node @p id; nothing for graph_input(), graph_output() and unknown ids.
    std::optional<float> wet_mix(NodeId id) const;
    /// The mix law set for node @p id; nothing for graph_input(), graph_output() and unknown ids.
    std::optional<MixLaw> mix_law(NodeId id) const;

    /// Every node, graph_input() and graph_output() included, as edited (not as committed).
    std::vector<NodeId> nodes() const;
    /// Every connection, as edited.
    std::vector<Connection> connections() const;
    /// The nodes connected into @p id, as edited.
    std::vector<NodeId> sources_of(NodeId id) const;

    // ─── BaseProcessor ───────────────────────
    /// Prepares every node and commits, including pending edits. Call only while process() is not
    /// running.
    void prepare(const double& sample_rate,
                 const size_t& samples_per_block,
                 const size_t& num_channels) override;

    /// @p modulation_offset: the position of @p buffer in the block when the caller runs the
    /// block in parts; every node gets it, so it reads its modulation at the block position.
    void process(thl::core::BufferView buffer,
                 uint32_t modulation_offset = 0) TANH_NONBLOCKING_FUNCTION override;

    /// Audio thread: resets every node of the running version and clears the latency delays,
    /// e.g. when the graph is itself a bypassed node coming back.
    void reset() TANH_NONBLOCKING_FUNCTION override;

private:
    struct MixGains {
        float m_dry;
        float m_wet;
    };
    /// The dry and wet gains of @p law at @p wet_mix (clamped to [0, 1]); exact at the ends.
    static MixGains mix_gains(MixLaw law, float wet_mix) TANH_NONBLOCKING_FUNCTION;

    using BufferIndex = uint32_t;
    struct Node;
    struct Source;
    struct ProcessingStep;
    struct CompiledGraph;

    struct RetiredNode {
        std::unique_ptr<Node> m_node;
        uint64_t m_last_generation;  // newest compiled version that may still use it
    };

    struct RetiredDelay {
        std::unique_ptr<thl::dsp::utils::DelayProcessor> m_delay;
        uint64_t m_last_generation;  // newest compiled version that may still use it
    };

    bool exists(NodeId id) const { return m_nodes.contains(id); }
    bool path_exists(NodeId from, NodeId to) const;
    std::optional<NodeId> insert_node(std::unique_ptr<thl::dsp::BaseProcessor> processor,
                                      bool prepare_processor);
    void prepare_node(Node& node, bool prepare_processor = true) const;
    void prepare_delays(std::span<Source> sources,
                        std::span<const uint32_t> max_path_latencies) const;
    bool max_latencies_changed() const;

    std::optional<std::vector<NodeId>> sort_graph_dfs() const;
    std::unique_ptr<CompiledGraph> compile();
    bool commit_own();
    bool rebuild();

    // This graph first, then every graph nested in it, at any depth.
    template <typename Self>
    static std::vector<Self*> graph_tree(Self& root);

    void publish(std::unique_ptr<CompiledGraph> compiled);
    void collect_own_garbage();
    void free_all_compiled();

    bool switch_to_pending() TANH_NONBLOCKING_FUNCTION;
    static uint32_t apply_latencies(CompiledGraph& compiled) TANH_NONBLOCKING_FUNCTION;
    void apply_fade(thl::core::BufferView buffer) TANH_NONBLOCKING_FUNCTION;
    static void mix_sources(const CompiledGraph& compiled,
                            std::span<Source> sources,
                            Node& node,
                            thl::core::BufferView destination,
                            thl::core::BufferView dry) TANH_NONBLOCKING_FUNCTION;
    static void run_step(const ProcessingStep& step,
                         thl::core::BufferView buffer,
                         thl::core::BufferView dry,
                         uint32_t modulation_offset) TANH_NONBLOCKING_FUNCTION;

    static constexpr NodeId k_output_id{0};
    static constexpr NodeId k_input_id{1};
    static constexpr double k_fade_seconds = 0.01;
    uint64_t m_next_id = 2;

    // ─── Control thread ──────────────────────
    std::map<NodeId, std::unique_ptr<Node>> m_nodes;
    std::set<Connection> m_connections;
    std::vector<RetiredNode> m_retired_nodes;
    std::vector<RetiredDelay> m_retired_delays;
    double m_sample_rate = 0.0;
    size_t m_max_block_size = 0;
    size_t m_num_channels = 0;
    bool m_dirty = false;
    bool m_published = false;          // a version was published since prepare()
    uint32_t m_committed_latency = 0;  // of the version published last

    uint64_t m_next_generation = 1;
    std::set<uint64_t> m_live_generations;  // handed to the audio thread and not yet freed

    // ─── Control thread -> audio thread ──────
    std::atomic<CompiledGraph*> m_pending{nullptr};
    thl::core::LockFreeQueue<CompiledGraph*, 8> m_returned;  // audio thread -> control thread

    // ─── Audio thread ────────────────────────
    CompiledGraph* m_active = nullptr;
    thl::dsp::utils::SmoothedValue m_fade;
    size_t m_fade_samples = 0;
    bool m_fading_out = false;
    size_t m_hold_samples = 0;  // silence after a switch, until the latency has passed
};

}  // namespace thl::graph
