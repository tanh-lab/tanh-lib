// SPDX-License-Identifier: Apache-2.0
#include <tanh/core/threading/RCU.h>

#include <atomic>

namespace thl::detail {

RcuThreadState::~RcuThreadState() {
    // Mark all nodes as dead - they'll be cleaned up by the respective RCU
    // instances (cleanup_dead_nodes / ~RCU).
    //
    // No need to wait for m_read_generation == 0: this thread is exiting, so
    // it cannot be inside a read section. synchronize_rcu() re-checks the
    // dead flag while waiting, so a stale generation cannot pin a writer.
    for (auto& [_, node] : m_nodes) {
        // Ownership passes to the RCU instance the node is linked into.
        if (RcuReaderNode* raw = node.release()) {
            raw->m_is_dead.store(true, std::memory_order_release);
        }
    }
}

// One instance per thread for the whole process. Defined here (not as a
// function-local static inside the RCU<T> template) so that every module —
// each DLL and the executable on Windows — resolves to the same thread_local.
RcuThreadState& rcu_thread_state() {
    static thread_local RcuThreadState instance;
    return instance;
}

}  // namespace thl::detail
