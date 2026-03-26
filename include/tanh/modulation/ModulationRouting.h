#pragma once

#include <cstdint>
#include <string>

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
};

}  // namespace thl::modulation
