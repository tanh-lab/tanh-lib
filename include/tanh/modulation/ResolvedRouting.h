#pragma once

#include <cstdint>

namespace thl::modulation {

class ModulationSource;
struct ResolvedTarget;

// Real-time resolved routing — raw pointers, no allocations.
// Created by ModulationMatrix during schedule rebuild.
struct ResolvedRouting {
    ModulationSource* m_source = nullptr;
    ResolvedTarget* m_target = nullptr;
    float m_depth = 1.0f;
    uint32_t m_max_decimation = 0;

    // Persistent counter for per-routing decimation override.
    // Counts down; when it reaches 0 a change point is forced.
    // Mutable because it is modified during RT processing through const
    // references obtained from RCU reads.
    mutable uint32_t m_samples_until_update = 0;
};

}  // namespace thl::modulation
