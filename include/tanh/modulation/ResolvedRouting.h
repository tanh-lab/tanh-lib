#pragma once

#include <cstdint>
#include <vector>

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
    CombineMode m_combine_mode = CombineMode::Additive;
    RoutingMode m_routing_mode = RoutingMode::MonoToMono;
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

    // Last active replace value, persists across blocks. Used by ReplaceHold
    // to continue replacing with the held value when the source becomes inactive.
    // Mutable for the same reason as m_samples_until_update.
    mutable float m_held_value = 0.0f;

    // Per-voice held values for polyphonic ReplaceHold routings.
    // Sized during schedule build. Mutable for RT processing.
    mutable std::vector<float> m_held_voice_values;
};

}  // namespace thl::modulation
