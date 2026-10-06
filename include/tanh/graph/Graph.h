#pragma once

#include <tanh/core/Exports.h>
#include <tanh/graph/Node.h>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <type_traits>
#include <map>
#include <utility>
#include <vector>
#include "tanh/core/threading/RCU.h"

namespace thl::graph {

enum class NodeId : uint64_t {};

struct PortRef {
    NodeId node{};
    uint32_t port = 0;
    friend auto operator<=>(const PortRef&, const PortRef&) = default;
};

class TANH_API Graph {
public:
    explicit Graph(size_t num_output_channels);
    ~Graph();

    Graph(const Graph&) = delete;
    Graph& operator=(const Graph&) = delete;
    Graph(Graph&&) = delete;
    Graph& operator=(Graph&&) = delete;

    // ─── Editing (control thread) ────────────
    template <typename T, typename... Args>
    std::optional<NodeId> add_node(Args&&... args) {
        static_assert(std::is_base_of_v<Node, T>);
        return add_node_impl(std::make_shared<T>(std::forward<Args>(args)...));
    }

    bool remove_node(NodeId id);
    bool connect(PortRef from, PortRef to);
    bool disconnect(PortRef to);

    NodeId graph_output() const { return k_output_id; }

    std::set<const void*> exclusive_resources() const;

    void prepare(const ProcessSpec& spec);

    bool commit();

    // ─── Processing (audio thread) ───────────
    void register_audio_thread() const;

    void process(thl::core::BufferView output) TANH_NONBLOCKING_FUNCTION;


private:
    using BufferIndex = uint32_t;
    static constexpr BufferIndex k_zeros_buffer = 0;
    struct GraphBuffers;
    struct ProcessingStep;
    struct CompiledGraph;

    std::optional<NodeId> add_node_impl(std::shared_ptr<Node> node);

    bool exists(NodeId id) const { return m_nodes.contains(id); }
    bool path_exists(NodeId from, NodeId to) const;

    std::optional<std::vector<NodeId>> sort_graph_dfs() const;
    std::optional<CompiledGraph> compile() const;

    static constexpr NodeId k_output_id{0};
    uint64_t m_next_id = 1;

    std::map<NodeId, std::shared_ptr<Node>> m_nodes;
    std::map<PortRef, PortRef> m_connections;  // destination -> source
    ProcessSpec m_spec{};

    thl::RCU<CompiledGraph> m_compiled_graph;
};

}  // namespace thl::graph