// SPDX-License-Identifier: Apache-2.0
#include <tanh/core/threading/RCU.h>

#include <atomic>

namespace thl::detail {

RcuThreadState::~RcuThreadState() {
    // Hand every node to its RCU instance (cleanup_dead_nodes / ~RCU frees it),
    // or free it here if the instance is already gone.
    //
    // No need to wait for m_read_generation == 0: this thread is exiting, so
    // it cannot be inside a read section. synchronize_rcu() re-checks the
    // state while waiting, so a stale generation cannot pin a writer.
    for (auto& [_, node] : m_nodes) {
        RcuReaderNode* raw = node.release();
        if (raw == nullptr) { continue; }
        if (raw->m_state.exchange(RcuNodeState::ThreadGone, std::memory_order_acq_rel) ==
            RcuNodeState::RcuGone) {
            delete raw;  // its RCU instance was destroyed first
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
