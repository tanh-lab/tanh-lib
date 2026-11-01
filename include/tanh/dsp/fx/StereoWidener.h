#pragma once

#include <tanh/core/AbiNamespace.h>
#include <tanh/core/Buffer.h>
#include <tanh/core/BufferView.h>
#include <tanh/core/Exports.h>
#include <tanh/dsp/utils/BandSplitter.h>
#include <tanh/dsp/utils/SmoothedValue.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace thl::inline THL_ABI_NAMESPACE::dsp::fx {

/**
 * @brief A multiband stereo widener: the side signal's level per band, the mid signal untouched.
 *
 * The stereo input is encoded to mid (L + R) / 2 and side (L - R) / 2, both split into
 * num_bands() bands by Linkwitz-Riley crossovers (BandSplitter), every band's side scaled by its
 * width, and decoded again. The mid signal only passes the crossovers' all-passes, as the side
 * does, so mid and side stay in phase: the output is the input through those all-passes (flat
 * magnitude) with each band as wide as its width says.
 *
 * - Width 0 makes a band mono (two bands, band 0 at 0: mono bass below the crossover), 1 leaves
 *   it as it is, 2 doubles its side signal; clamped to [0, k_max_width]. Every width glides
 *   over k_ramp_seconds.
 * - Mono compatible whatever the widths: L + R is the mid signal through the all-passes.
 * - No latency. process_with_dry() writes the input through the same all-passes, phase-matched,
 *   for a blend.
 * - Any channel count but 2 passes unchanged (nothing to widen).
 *
 * A derived class gives every band's width through get_band_parameter_float(), and may give the
 * crossover frequencies through get_crossover_parameter_float(); otherwise set_frequency() or
 * set_frequencies() set them. Both are read at the start of every process() call, at
 * @p modulation_offset.
 *
 * Threads: prepare() allocates; process(), process_with_dry() and reset() are real-time safe.
 */
class TANH_API StereoWidenerImpl : public thl::dsp::utils::BandSplitter {
public:
    static constexpr float k_max_width = 2.0f;
    static constexpr double k_ramp_seconds = 0.02;

    /// @p num_bands bands (at least 2), crossovers of Linkwitz-Riley @p order (2 to 8, even).
    explicit StereoWidenerImpl(size_t num_bands = 2, size_t order = 4);

    void prepare(const double& sample_rate,
                 const size_t& samples_per_block,
                 const size_t& num_channels) override;
    void process(thl::core::BufferView buffer,
                 uint32_t modulation_offset = 0) TANH_NONBLOCKING_FUNCTION override;
    void process_with_dry(thl::core::BufferView buffer,
                          uint32_t modulation_offset,
                          thl::core::BufferView dry) TANH_NONBLOCKING_FUNCTION override;
    /// Clears the crossovers and ends the width glides at their targets.
    void reset() TANH_NONBLOCKING_FUNCTION override;

protected:
    enum BandParameter {
        Width = 0,

        NumBandParameters = 1
    };

    virtual float get_band_parameter_float(BandParameter parameter,
                                           size_t band,
                                           uint32_t modulation_offset = 0) = 0;

    /// The frequency (Hz) of crossover @p crossover (0: between bands 0 and 1), read for every
    /// crossover at each process() call and applied together (set_frequencies()). Non-finite
    /// (the default): that crossover keeps what set_frequency() gave it.
    virtual float get_crossover_parameter_float(size_t crossover, uint32_t modulation_offset = 0);

private:
    void read_parameters(uint32_t modulation_offset) TANH_NONBLOCKING_FUNCTION;
    [[nodiscard]] float width_of(size_t band, uint32_t modulation_offset) TANH_NONBLOCKING_FUNCTION;

    std::vector<thl::dsp::utils::SmoothedValue> m_widths;  // per band
    std::vector<float> m_width_targets;                    // per band, as read last
    std::vector<float> m_crossover_parameters;             // one per crossover, read every block
    thl::core::Buffer<float> m_mid_side;                   // band 0 in place: mid, side
    thl::core::Buffer<float> m_band;                       // bands 1 and up
    thl::core::Buffer<float> m_wide;                       // the sum: mid, side as widened
    thl::core::Buffer<float> m_through;                    // the sum at width 1: the dry signal
};

}  // namespace thl::inline THL_ABI_NAMESPACE::dsp::fx
