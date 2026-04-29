#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace thl::modulation {

// Depth interpretation mode for modulation routings.
enum class DepthMode : uint8_t {
    Absolute,   // m_depth is in plain parameter units
    Normalized  // m_depth is a fraction of the parameter's normalized [0,1] range
};

// Combine mode: how the routing's output is applied to the target.
enum class CombineMode : uint8_t {
    Additive,    // buffer[i] += source[i] * depth  (default)
    Replace,     // replaces base value, falls back to base when source inactive
    ReplaceHold  // replaces base value, holds last active value when source inactive
};

// Routing mode: derived from source scope + target scope at schedule build.
// Stored on ResolvedRouting, not on ModulationRouting. The scope matrix:
//
//  src.scope \ tgt.scope |   Global    |   Scope S    | Scope T (≠ S)
//  ──────────────────────┼─────────────┼──────────────┼───────────────
//  Global                |GlobalToGlo..| GlobalToSco..| GlobalToScoped
//  Scope S               |ScopedToGlo..| ScopedToSco..| CrossScope
//
// ScopedToGlobal (scope narrowing — voice source into a mono-only parameter)
// and CrossScope (two unrelated voice domains) are both rejected at
// schedule-build time. They are separate enumerators rather than a single
// "Invalid" bucket because they represent semantically distinct rejection
// reasons and should log distinctly.
enum class RoutingMode : uint8_t {
    GlobalToGlobal,  // global source → global buffer
    GlobalToScoped,  // global source → broadcast to all voice buffers in tgt scope
    ScopedToScoped,  // voice source[v] → voice buffer[v] (1:1, same scope)
    ScopedToGlobal,  // rejected: scope-narrowing (voice → mono-only)
    CrossScope       // rejected: two different non-global scopes
};

// Sentinel value returned by add_routing() when the routing is rejected.
static constexpr uint32_t k_invalid_routing_id = 0;

// User-facing routing descriptor — uses string identifiers for source/target.
// Converted to ResolvedRouting by ModulationMatrix when the schedule is built.
struct ModulationRouting {
    // Unique routing ID assigned by ModulationMatrix::add_routing().
    // 0 (k_invalid_routing_id) means unassigned.
    uint32_t m_id = k_invalid_routing_id;

    std::string m_source_id;
    std::string m_target_id;
    float m_depth = 1.0f;
    DepthMode m_depth_mode = DepthMode::Normalized;

    CombineMode m_combine_mode = CombineMode::Additive;

    // Priority for multi-Replace composition on a shared target. Higher value
    // wins when multiple active Replace routings overlap. Default 0 — single
    // Replace routings behave identically regardless of priority value, so
    // legacy presets without the field load as 0 without behavior change.
    // Equal priorities fall back to add-order among active routings.
    // Only meaningful for Replace / ReplaceHold combine modes; ignored for
    // Additive.
    uint32_t m_replace_priority = 0;

    // Replace range — maps source [0,1] to [min, max] in plain parameter units.
    // Only meaningful for Replace/ReplaceHold combine modes.
    float m_replace_range_min = 0.0f;
    float m_replace_range_max = 1.0f;
    bool m_has_replace_range = false;

    // Maximum decimation factor for this routing. The modulation matrix will
    // guarantee at least one change point every max_decimation samples along
    // this routing. 0 means use the source's native resolution.
    uint32_t m_max_decimation = 0;

    // When true, this routing is skipped while the target parameter is in gesture
    // (i.e. the user is actively interacting with the parameter).
    bool m_skip_during_gesture = false;

    ModulationRouting() = default;

    ModulationRouting(std::string_view view_source_id,
                      std::string_view view_target_id,
                      float new_depth = 1.0f,
                      uint32_t new_max_decimation = 0,
                      DepthMode new_depth_mode = DepthMode::Normalized,
                      CombineMode new_combine_mode = CombineMode::Additive) {
        m_source_id = std::string(view_source_id);
        m_target_id = std::string(view_target_id);
        m_depth = new_depth;
        m_max_decimation = new_max_decimation;
        m_depth_mode = new_depth_mode;
        m_combine_mode = new_combine_mode;
    }
};

}  // namespace thl::modulation
