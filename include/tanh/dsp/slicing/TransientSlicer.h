#pragma once

#include <tanh/core/Buffer.h>
#include <tanh/core/Exports.h>
#include <tanh/dsp/slicing/SliceMap.h>

#include <cstddef>
#include <vector>

namespace thl::dsp::slicing {

/**
 * @brief Onset-strength curve of one sample, one value per hop.
 *
 * Computed once per sample by TransientSlicer::analyse(); picking a
 * different slice count only re-runs TransientSlicer::pick().
 */
struct TransientAnalysis {
    std::vector<float> m_strength;  ///< >= 0 per hop.
    size_t m_hop_frames{256};
    size_t m_num_frames{0};  ///< Frames of the analysed sample.
    double m_sample_rate{0.0};
    [[nodiscard]] bool empty() const { return m_strength.empty() || m_num_frames == 0; }
};

/**
 * @class TransientSlicer
 * @brief Offline transient-based slicing of a sample into N slices.
 *
 * @par Analysis
 * Band-energy flux: mono mixdown, split into low / mid / high with two
 * state-variable filters (crossovers `m_low_hz`, `m_high_hz`), RMS per hop
 * over a trailing window, log-compressed per band, positive first difference
 * summed over the bands, a local mean subtracted and half-wave rectified.
 *
 * @par Picking
 * The N-1 strongest local maxima at least `m_min_slice_seconds` apart (and
 * from both ends) become boundaries. With the audio at hand each is refined:
 * moved to the sharpest short-time energy rise in its hop, back up to
 * `m_move_back_seconds` to the quietest frame before the attack, and onto a
 * zero crossing within `m_zero_cross_seconds`. With fewer peaks than
 * needed, the longest gaps are split evenly. Silence or a sample too short
 * for N slices gives the grid.
 *
 * @par Real-Time Safety
 * Not real-time safe: allocates and scans the whole sample. Run it on a
 * loader or worker thread and hand the resulting SliceMap over.
 */
class TANH_API TransientSlicer {
public:
    struct Settings {
        size_t m_hop_frames{256};
        size_t m_window_hops{4};  ///< Trailing RMS window, in hops.
        float m_low_hz{160.0f};
        float m_high_hz{1600.0f};
        double m_log_gain{1000.0};  ///< log1p(gain * rms).
        size_t m_local_mean_hops{10};
        double m_min_slice_seconds{0.030};
        double m_move_back_seconds{0.005};
        double m_zero_cross_seconds{0.002};
        double m_energy_window_seconds{0.001};
    };

    TransientSlicer() = default;
    explicit TransientSlicer(const Settings& settings) : m_settings(settings) {}

    [[nodiscard]] const Settings& settings() const { return m_settings; }

    /// Onset-strength curve of `buffer` (all channels mixed to mono).
    [[nodiscard]] TransientAnalysis analyse(const core::BufferF& buffer, double sample_rate) const;

    /**
     * @brief Pick `count` slices from an analysis.
     * @param refine The analysed audio, to refine boundaries against (see
     *               class); nullptr keeps the hop-resolution peaks.
     * @return A valid map of `count` slices (clamped to 1..k_max_slices).
     */
    [[nodiscard]] SliceMap pick(const TransientAnalysis& analysis,
                                size_t count,
                                const core::BufferF* refine = nullptr) const;

private:
    Settings m_settings{};
};

}  // namespace thl::dsp::slicing
