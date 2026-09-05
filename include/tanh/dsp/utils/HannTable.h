#pragma once

#include <tanh/core/Exports.h>

#include <array>
#include <cstddef>

namespace thl::dsp::utils {

/**
 * @brief Hann window sampled into a table, for callers that evaluate it
 *        per sample per voice (a granular pool of 48 grains asks for it
 *        2.3 M times a second at 48 kHz).
 *
 * 4096 segments with linear interpolation: worst-case error ~2e-8, below
 * float resolution of the window. `shared()` is one process-wide instance
 * built at static initialisation, so no audio thread ever pays for it.
 * HannWindow stays the exact, clocked window for everything else.
 */
class TANH_API HannTable {
public:
    static constexpr size_t k_segments = 4096;

    HannTable();

    /// Window value at `phase` in [0, 1); clamped outside.
    float at(float phase) const;

    /// The formula the table samples: 0.5 * (1 - cos(2 pi phase)).
    static float exact(float phase);

    /// The process-wide table.
    static const HannTable& shared();

private:
    std::array<float, k_segments + 1> m_table{};
};

}  // namespace thl::dsp::utils
