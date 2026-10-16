#pragma once

#include <cstdint>
#include <optional>
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

// Times of the host hand-over (ModulationRouting::m_share_with_host), set per
// matrix with ModulationMatrix::set_host_share_times(). Negative values count
// as 0; a ramp of 0 ms switches within one sample.
struct HostShareTimes {
    static constexpr float k_take_ms = 20.0f;
    static constexpr float k_return_ms = 50.0f;
    static constexpr float k_hold_ms = 300.0f;

    // While the hand-over weight moves, the matrix flags a change point on the
    // target at least every this many samples (and where the ramp starts and ends).
    static constexpr uint32_t k_change_point_interval = 32;

    // Ramp of the weight from the host to the modulation (linear).
    float m_take_ms = k_take_ms;
    // Ramp of the weight from the modulation back to the host (linear).
    float m_return_ms = k_return_ms;
    // How long the host stays active after its last base-value change (or the
    // end of a gesture).
    float m_hold_ms = k_hold_ms;
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
    //
    // At equal priority, the matrix breaks ties by freshness:
    //   1. A live writer always beats a held writer at the same priority.
    //   2. Among two live writers, the one whose current live phase started
    //      later (more recent inactive→active transition) wins.
    //   3. Among two held writers, the one whose source was most recently
    //      active wins.
    //   4. If even freshness ties (e.g., both start the same live phase at
    //      the same sample), schedule order — the later-scheduled source
    //      overwrites the earlier one within that sample.
    // Only meaningful for Replace / ReplaceHold combine modes; ignored for
    // Additive.
    uint32_t m_replace_priority = 0;

    // Separate priority for the held-write branch of ReplaceHold. When the
    // source goes inactive but has previously been active, the routing
    // continues writing m_held_value at this priority instead of
    // m_replace_priority — so a held source can yield to a now-active
    // lower-priority peer (or vice-versa) without the user re-arming
    // priorities at runtime. std::nullopt (the default) inherits
    // m_replace_priority, preserving the pre-feature behavior. Ignored for
    // plain Replace and Additive combine modes.
    std::optional<uint32_t> m_replace_hold_priority;

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

    // Share the target with the host, as a DAW's Touch automation mode does
    // (Replace / ReplaceHold from a global source into a global target only).
    // The host (its base value: automation, a gesture) and the target's Replace
    // output are two writers; the last one to become active owns the
    // parameter, and when the owner stops, the other takes over if it is
    // active, else the owner keeps it. Every hand-over is a crossfade with the
    // times of ModulationMatrix::set_host_share_times(). Host activity comes
    // from ParameterBinding::m_host_change_stamp and m_in_gesture.
    //
    // Sharing is a property of the target: one routing that opts in makes
    // every Replace / ReplaceHold routing on that target one writer (their
    // composed output), and m_skip_during_gesture is ignored on it (a gesture
    // is host activity). A polyphonic target is not shared (logged warning).
    // See "Sharing a parameter with the host" in host_parameters.md.
    bool m_share_with_host = false;

    // A disabled routing writes nothing to its target and drops its held
    // value. Unlike a depth of 0 this also silences Replace / ReplaceHold.
    // Toggled without a rebuild by ModulationMatrix::set_routing_enabled();
    // serialised only when false, so older presets load as enabled.
    bool m_enabled = true;

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
