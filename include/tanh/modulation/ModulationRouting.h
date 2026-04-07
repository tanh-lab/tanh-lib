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

// Routing mode: auto-detected from source capability + target polyphony.
// Stored on ResolvedRouting, not on ModulationRouting.
enum class RoutingMode : uint8_t {
    MonoToMono,  // global source → global buffer
    PolyToPoly,  // voice source[v] → voice buffer[v] (1:1)
    MonoToPoly,  // global source → broadcast to all voice buffers
    PolyToMono   // rejected at schedule-build time
};

// User-facing routing descriptor — uses string identifiers for source/target.
// Converted to ResolvedRouting by ModulationMatrix when the schedule is built.
struct ModulationRouting {
    std::string m_source_id;
    std::string m_target_id;
    float m_depth = 1.0f;
    DepthMode m_depth_mode = DepthMode::Normalized;

    CombineMode m_combine_mode = CombineMode::Additive;

    // Maximum decimation factor for this routing. The modulation matrix will
    // guarantee at least one change point every max_decimation samples along
    // this routing. 0 means use the source's native resolution.
    uint32_t m_max_decimation = 0;

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
