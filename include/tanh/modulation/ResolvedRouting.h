#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include "ModulationRouting.h"

namespace thl::modulation {

class ModulationSource;
struct ResolvedTarget;

// Real-time resolved routing — raw pointers, no allocations.
// Created by ModulationMatrix during schedule rebuild.
//
// Fields marked mutable std::atomic are written by the UI thread (via
// update_routing_depth / update_routing_replace_range) and read by the RT
// thread during process(). memory_order_relaxed is used for both sides —
// on ARM64 and x86 this compiles to plain loads/stores for naturally aligned
// 32-bit values, so there is zero overhead compared to non-atomic access.
struct ResolvedRouting {
    // Unique routing ID — matches the ModulationRouting::m_id it was resolved from.
    uint32_t m_id = k_invalid_routing_id;

    ModulationSource* m_source = nullptr;
    ResolvedTarget* m_target = nullptr;

    // Raw user depth — used by the replace-with-range path at RT.
    mutable std::atomic<float> m_depth{1.0f};
    DepthMode m_depth_mode = DepthMode::Normalized;
    CombineMode m_combine_mode = CombineMode::Additive;
    RoutingMode m_routing_mode = RoutingMode::GlobalToGlobal;
    uint32_t m_max_decimation = 0;
    uint32_t m_priority = 0;
    bool m_skip_during_gesture = false;

    // True if this routing is deferred to the post-schedule Replace-composition
    // pass. Set at schedule-build time when the target has >1 Replace routing.
    // Single-Replace targets keep the in-schedule fast path (m_replace_deferred
    // stays false).
    bool m_replace_deferred = false;

    // Pre-computed per-sample depth multiplier, set at schedule-build time.
    // For linear targets: depth * (max - min) converts normalized depth to plain units.
    // For non-linear targets (additive only): depth in normalized space.
    // Replace routings always use plain-unit precomputation.
    mutable std::atomic<float> m_depth_abs_precomputed{0.0f};

    // Replace range — maps source [0,1] to [min, max] in plain parameter units.
    // Only meaningful when m_has_replace_range is true and combine mode is
    // Replace or ReplaceHold. Written by UI thread, read by RT thread.
    mutable std::atomic<float> m_replace_range_min{0.0f};
    mutable std::atomic<float> m_replace_range_max{1.0f};
    mutable std::atomic<bool> m_has_replace_range{false};

    // Persistent counter for per-routing decimation override.
    // Counts down; when it reaches 0 a change point is forced.
    // Mutable because it is modified during RT processing through const
    // references obtained from RCU reads.
    mutable uint32_t m_samples_until_update = 0;

    // Last active replace value, persists across blocks. Used by ReplaceHold
    // to continue replacing with the held value when the source becomes inactive.
    // Mutable for the same reason as m_samples_until_update.
    mutable float m_held_value = 0.0f;

    // True once the source has been active at least once for this routing
    // (mono path). Gates the ReplaceHold fallback so untouched sources don't
    // broadcast active=1 across the target — the fallback only holds real
    // contributions. Mutable for RT processing.
    mutable bool m_held_mono_active = false;

    // Per-voice held values for polyphonic ReplaceHold routings.
    // Sized during schedule build. Mutable for RT processing.
    mutable std::vector<float> m_held_voice_values;

    // Per-voice "has ever been active" bitmap for polyphonic ReplaceHold
    // routings. Parallel to m_held_voice_values. Gates the per-voice
    // ReplaceHold fallback so voices that have never received a live
    // contribution stay inactive instead of holding the default-0 value.
    mutable std::vector<uint8_t> m_held_voice_active;

    // ── Copy semantics (required for std::vector in RCU ProcessingConfig) ──

    ResolvedRouting() = default;

    ResolvedRouting(const ResolvedRouting& other)
        : m_id(other.m_id)
        , m_source(other.m_source)
        , m_target(other.m_target)
        , m_depth(other.m_depth.load(std::memory_order_relaxed))
        , m_depth_mode(other.m_depth_mode)
        , m_combine_mode(other.m_combine_mode)
        , m_routing_mode(other.m_routing_mode)
        , m_max_decimation(other.m_max_decimation)
        , m_priority(other.m_priority)
        , m_skip_during_gesture(other.m_skip_during_gesture)
        , m_replace_deferred(other.m_replace_deferred)
        , m_depth_abs_precomputed(other.m_depth_abs_precomputed.load(std::memory_order_relaxed))
        , m_replace_range_min(other.m_replace_range_min.load(std::memory_order_relaxed))
        , m_replace_range_max(other.m_replace_range_max.load(std::memory_order_relaxed))
        , m_has_replace_range(other.m_has_replace_range.load(std::memory_order_relaxed))
        , m_samples_until_update(other.m_samples_until_update)
        , m_held_value(other.m_held_value)
        , m_held_mono_active(other.m_held_mono_active)
        , m_held_voice_values(other.m_held_voice_values)
        , m_held_voice_active(other.m_held_voice_active) {}

    ResolvedRouting& operator=(const ResolvedRouting& other) {
        if (this != &other) {
            m_id = other.m_id;
            m_source = other.m_source;
            m_target = other.m_target;
            m_depth.store(other.m_depth.load(std::memory_order_relaxed), std::memory_order_relaxed);
            m_depth_mode = other.m_depth_mode;
            m_combine_mode = other.m_combine_mode;
            m_routing_mode = other.m_routing_mode;
            m_max_decimation = other.m_max_decimation;
            m_priority = other.m_priority;
            m_skip_during_gesture = other.m_skip_during_gesture;
            m_replace_deferred = other.m_replace_deferred;
            m_depth_abs_precomputed.store(
                other.m_depth_abs_precomputed.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
            m_replace_range_min.store(other.m_replace_range_min.load(std::memory_order_relaxed),
                                      std::memory_order_relaxed);
            m_replace_range_max.store(other.m_replace_range_max.load(std::memory_order_relaxed),
                                      std::memory_order_relaxed);
            m_has_replace_range.store(other.m_has_replace_range.load(std::memory_order_relaxed),
                                      std::memory_order_relaxed);
            m_samples_until_update = other.m_samples_until_update;
            m_held_value = other.m_held_value;
            m_held_mono_active = other.m_held_mono_active;
            m_held_voice_values = other.m_held_voice_values;
            m_held_voice_active = other.m_held_voice_active;
        }
        return *this;
    }
};

}  // namespace thl::modulation
