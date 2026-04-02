#pragma once

#include <cstdint>
#include <vector>

namespace thl::modulation {

// Per-parameter modulation target. Owns the modulation buffer, change point
// flags (bitset), and the final sorted change point list built from the flags.
struct ResolvedTarget {
    std::string id;

    // Per-sample modulation buffer. Filled by routings; summed over all sources:
    //   final_value[i] = base_value + modulation_buffer[i]
    std::vector<float> modulation_buffer;

    // Bitset flags — one per sample. Sources set flags (idempotent).
    // After all sources have run, a linear scan builds the sorted change point
    // list from these flags. This avoids the overflow bug of appending + sort.
    std::vector<bool> change_point_flags;

    // Final sorted change point list, built from change_point_flags.
    std::vector<uint32_t> change_points;

    void resize(size_t num_samples) {
        modulation_buffer.assign(num_samples, 0.0f);
        change_point_flags.assign(num_samples, false);
        change_points.clear();
        change_points.reserve(num_samples);
    }

    void clear_per_block() {
        std::fill(modulation_buffer.begin(), modulation_buffer.end(), 0.0f);
        std::fill(change_point_flags.begin(), change_point_flags.end(), false);
        change_points.clear();
    }

    // Build sorted change_points from the flags bitset.
    void build_change_points() {
        change_points.clear();
        for (size_t i = 0; i < change_point_flags.size(); ++i) {
            if (change_point_flags[i]) { change_points.push_back(static_cast<uint32_t>(i)); }
        }
    }
};

}  // namespace thl::modulation
