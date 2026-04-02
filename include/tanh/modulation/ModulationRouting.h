#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace thl::modulation {

// User-facing routing descriptor — uses string identifiers for source/target.
// Converted to ResolvedRouting by ModulationMatrix when the schedule is built.
struct ModulationRouting {
    std::string source_id;
    std::string target_id;
    float depth = 1.0f;

    // Maximum decimation factor for this routing. The modulation matrix will
    // guarantee at least one change point every max_decimation samples along
    // this routing. 0 means use the source's native resolution.
    uint32_t max_decimation = 0;

    ModulationRouting() = default;

    ModulationRouting(std::string_view view_source_id,
                      std::string_view view_target_id,
                      float new_depth = 1.0f,
                      uint32_t new_max_decimation = 0) {
        source_id = std::string(view_source_id);
        target_id = std::string(view_target_id);
        depth = new_depth;
        max_decimation = new_max_decimation;
    }
};

}  // namespace thl::modulation
