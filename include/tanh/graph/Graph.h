#pragma once

#include <tanh/core/BufferView.h>
#include <tanh/core/Exports.h>
#include <tanh/core/threading/LockFreeQueue.h>
#include <tanh/dsp/BaseProcessor.h>
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
/// Edits are made on the control thread and take effect at commit(). The audio thread switches to
/// the committed version at its next block.
class TANH_API Graph final : public thl::dsp::BaseProcessor {
public:
    /// How a node combines its sources.
    enum class MixMode : uint8_t { Sum, Average };

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

    std::optional<NodeId> add_node(std::unique_ptr<thl::dsp::BaseProcessor> processor);

    bool remove_node(NodeId id);
    bool connect(NodeId from, NodeId to);
    bool disconnect(NodeId from, NodeId to);
    bool set_mix_mode(NodeId id, MixMode mode);

    bool commit();
    bool has_uncommitted_changes() const { return m_dirty; }

    NodeId graph_input() const { return k_input_id; }
    NodeId graph_output() const { return k_output_id; }

    /// nullptr for graph_input(), graph_output() and unknown ids.
    thl::dsp::BaseProcessor* node(NodeId id);
    const thl::dsp::BaseProcessor* node(NodeId id) const;

    std::vector<NodeId> nodes() const;
    std::vector<Connection> connections() const;
    std::vector<NodeId> sources_of(NodeId id) const;

    // ─── BaseProcessor ───────────────────────
    /// Prepares every node and commits, including pending edits. Call only while process() is not
    /// running.
    void prepare(const double& sample_rate,
                 const size_t& samples_per_block,
                 const size_t& num_channels) override;

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

    std::optional<std::vector<NodeId>> sort_graph_dfs() const;
    std::unique_ptr<CompiledGraph> compile() const;

    void publish(std::unique_ptr<CompiledGraph> compiled);
    void collect_garbage();
    void free_all_compiled();

    void take_pending() TANH_NONBLOCKING_FUNCTION;
    static void mix_sources(const CompiledGraph& compiled,
                            std::span<const BufferIndex> sources,
                            MixMode mode,
                            thl::core::BufferView destination) TANH_NONBLOCKING_FUNCTION;

    static constexpr NodeId k_output_id{0};
    static constexpr NodeId k_input_id{1};
    uint64_t m_next_id = 2;

    // ─── Control thread ──────────────────────
    std::map<NodeId, std::unique_ptr<Node>> m_nodes;
    std::set<Connection> m_connections;
    std::vector<RetiredNode> m_retired_nodes;
    double m_sample_rate = 0.0;
    size_t m_max_block_size = 0;
    size_t m_num_channels = 0;
    bool m_dirty = false;

    uint64_t m_next_generation = 1;
    std::set<uint64_t> m_live_generations;  // handed to the audio thread and not yet freed

    // ─── Control thread -> audio thread ──────
    std::atomic<CompiledGraph*> m_pending{nullptr};
    thl::core::LockFreeQueue<CompiledGraph*, 8> m_returned;  // audio thread -> control thread

    // ─── Audio thread ────────────────────────
    CompiledGraph* m_active = nullptr;
};

}  // namespace thl::graph
