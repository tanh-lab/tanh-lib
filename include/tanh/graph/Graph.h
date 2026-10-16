#pragma once

#include <tanh/core/Buffer.h>
#include <tanh/core/BufferView.h>
#include <tanh/core/Exports.h>
#include <tanh/core/threading/LockFreeQueue.h>
#include <tanh/dsp/BaseProcessor.h>
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

enum class NodeId : uint64_t {};

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
/// out the current version, switches to the committed one and fades it in.
class TANH_API Graph final : public thl::dsp::BaseProcessor {
public:
    /// How a node combines its sources.
    enum class InputMix : uint8_t { Sum, Average };

    Graph();
    ~Graph() override;

    Graph(const Graph&) = delete;
    Graph& operator=(const Graph&) = delete;
    Graph(Graph&&) = delete;
    Graph& operator=(Graph&&) = delete;

    // ─── Editing (control thread) ────────────
    template <typename T, typename... Args>
    std::optional<NodeId> add_node(Args&&... args) {
        static_assert(std::is_base_of_v<thl::dsp::BaseProcessor, T>);
        return add_node(std::make_unique<T>(std::forward<Args>(args)...));
    }

    /// A prepared graph prepares the processor here, on the calling thread.
    std::optional<NodeId> add_node(std::unique_ptr<thl::dsp::BaseProcessor> processor);

    /// A processor the caller already prepared, e.g. a heavy one on its own background thread,
    /// for @p sample_rate, @p samples_per_block and @p num_channels. The graph prepares it again
    /// only if they differ from its own (or prepare() runs again).
    std::optional<NodeId> add_prepared_node(std::unique_ptr<thl::dsp::BaseProcessor> processor,
                                            double sample_rate,
                                            size_t samples_per_block,
                                            size_t num_channels);

    bool remove_node(NodeId id);
    bool connect(NodeId from, NodeId to);
    bool disconnect(NodeId from, NodeId to);
    bool set_input_mix(NodeId id, InputMix input_mix);
    bool set_wet_mix(NodeId id, float wet_mix);

    /// Hands the edited graph to the audio thread (a short fade out and in). Without changes
    /// since the last commit it does nothing. False if the graph has a cycle.
    bool commit();
    bool has_uncommitted_changes() const { return m_dirty; }

    /// Frees the versions the audio thread handed back and the removed nodes no running version
    /// uses, also in nested graphs. commit() calls it; call it from a timer (e.g. 30 Hz on the
    /// message thread) so removed processors go away without another edit. A nested graph that
    /// does not run (wet 0) keeps its old version until it runs again.
    void collect_garbage();

    NodeId graph_input() const { return k_input_id; }
    NodeId graph_output() const { return k_output_id; }

    /// nullptr for graph_input(), graph_output() and unknown ids.
    thl::dsp::BaseProcessor* node(NodeId id);
    const thl::dsp::BaseProcessor* node(NodeId id) const;
    std::optional<float> wet_mix(NodeId id) const;

    std::vector<NodeId> nodes() const;
    std::vector<Connection> connections() const;
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

private:
    using BufferIndex = uint32_t;
    struct Node;
    struct ProcessingStep;
    struct CompiledGraph;

    struct RetiredNode {
        std::unique_ptr<Node> m_node;
        uint64_t m_last_generation;  // newest compiled version that may still use it
    };

    bool exists(NodeId id) const { return m_nodes.contains(id); }
    bool path_exists(NodeId from, NodeId to) const;
    std::optional<NodeId> insert_node(std::unique_ptr<thl::dsp::BaseProcessor> processor,
                                      bool prepare_processor);
    void prepare_node(Node& node, bool prepare_processor = true) const;

    std::optional<std::vector<NodeId>> sort_graph_dfs() const;
    std::unique_ptr<CompiledGraph> compile();
    bool rebuild();

    void publish(std::unique_ptr<CompiledGraph> compiled);
    void collect_own_garbage();
    void recycle(CompiledGraph* compiled);
    [[nodiscard]] thl::core::Buffer<float> take_buffer();
    void free_all_compiled();

    bool switch_to_pending() TANH_NONBLOCKING_FUNCTION;
    void apply_fade(thl::core::BufferView buffer) TANH_NONBLOCKING_FUNCTION;
    static void mix_sources(const CompiledGraph& compiled,
                            std::span<const BufferIndex> sources,
                            Node& node,
                            thl::core::BufferView destination) TANH_NONBLOCKING_FUNCTION;
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
    double m_sample_rate = 0.0;
    size_t m_max_block_size = 0;
    size_t m_num_channels = 0;
    bool m_dirty = false;
    bool m_published = false;  // a version was published since prepare()
    // Buffers of freed versions, sized for prepare(): compile() takes them first.
    std::vector<thl::core::Buffer<float>> m_buffer_pool;

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
};

}  // namespace thl::graph
