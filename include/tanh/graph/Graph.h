#pragma once

#include <tanh/core/Exports.h>
#include <tanh/graph/Node.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <map>
#include <utility>
#include "tanh/core/threading/RCU.h"

namespace thl::graph {

enum class NodeId : uint64_t {};

struct PortRef {
    NodeId node{};
    uint32_t port = 0;
    friend bool operator==(const PortRef&, const PortRef&) = default;
};
 
struct Connection {
    PortRef from;
    PortRef to;
    friend bool operator==(const Connection&, const Connection&) = default;
};

struct CompiledGraph;

class TANH_API Graph {
public:
    Graph(size_t num_input_channels, size_t num_output_channels);
    ~Graph();

    Graph(const Graph&) = delete;
    Graph& operator=(const Graph&) = delete;
    Graph(Graph&&) = delete;
    Graph& operator=(Graph&&) = delete;

    // ─── Editing (control thread) ────────────
    template <typename T, typename... Args>
    NodeId add_node(Args&&... args) {
        static_assert(std::is_base_of_v<Node, T>);
        return add_node_impl(std::make_shared<T>(std::forward<Args>(args)...));
    }

    bool remove_node(NodeId id);
    bool connect(PortRef from, PortRef to);
    bool disconnect(PortRef from, PortRef to);

    NodeId graph_input() const { return k_input_id; }
    NodeId graph_output() const { return k_output_id; }

    void prepare(const ProcessSpec& spec);

    bool commit();

    // ─── Processing (audio thread) ───────────
    void register_audio_thread() const;

    void process(thl::core::ConstBufferView input,
                 thl::core::BufferView output) TANH_NONBLOCKING_FUNCTION;


private:
    NodeId add_node_impl(std::shared_ptr<Node> node);

    bool exists(NodeId id) const { return m_nodes.contains(id); }
    bool path_exists(NodeId from, NodeId to) const;

    static constexpr NodeId k_input_id{1};
    static constexpr NodeId k_output_id{2};
    uint64_t m_next_id = 3;

    std::map<NodeId, std::shared_ptr<Node>> m_nodes;
    std::vector<Connection> m_connections;
    ProcessSpec m_spec{};

    thl::RCU<CompiledGraph> m_compiled_graph;
};

}  // namespace thl::graph