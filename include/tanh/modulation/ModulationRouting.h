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

// User-facing routing descriptor — uses string identifiers for source/target.
// Converted to ResolvedRouting by ModulationMatrix when the schedule is built.
struct ModulationRouting {
    std::string m_source_id;
    std::string m_target_id;
    float m_depth = 1.0f;
    DepthMode m_depth_mode = DepthMode::Normalized;

    // Maximum decimation factor for this routing. The modulation matrix will
    // guarantee at least one change point every max_decimation samples along
    // this routing. 0 means use the source's native resolution.
    uint32_t m_max_decimation = 0;

    ModulationRouting() = default;

    ModulationRouting(std::string_view view_source_id,
                      std::string_view view_target_id,
                      float new_depth = 1.0f,
                      uint32_t new_max_decimation = 0,
                      DepthMode new_depth_mode = DepthMode::Normalized) {
        m_source_id = std::string(view_source_id);
        m_target_id = std::string(view_target_id);
        m_depth = new_depth;
        m_max_decimation = new_max_decimation;
        m_depth_mode = new_depth_mode;
    }
};

}  // namespace thl::modulation
