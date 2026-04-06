#pragma once

#include <cstdint>

#include "ModulationRouting.h"

namespace thl::modulation {

class ModulationSource;
struct ResolvedTarget;

// Real-time resolved routing — raw pointers, no allocations.
// Created by ModulationMatrix during schedule rebuild.
struct ResolvedRouting {
    ModulationSource* m_source = nullptr;
    ResolvedTarget* m_target = nullptr;
    float m_depth = 1.0f;
    DepthMode m_depth_mode = DepthMode::Normalized;
    uint32_t m_max_decimation = 0;

    // Pre-computed per-sample depth multiplier, set at schedule-build time.
    // For linear targets: depth * (max - min) converts normalized depth to plain units.
    // For non-linear targets: depth in normalized space (Absolute depths are
    // converted via depth / (max - min)).
    // For absolute depth on linear targets: unused (m_depth is used directly).
    float m_depth_abs_precomputed = 0.0f;

    // Persistent counter for per-routing decimation override.
    // Counts down; when it reaches 0 a change point is forced.
    // Mutable because it is modified during RT processing through const
    // references obtained from RCU reads.
    mutable uint32_t m_samples_until_update = 0;
};

}  // namespace thl::modulation
