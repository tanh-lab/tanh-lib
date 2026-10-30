#pragma once

#include <tanh/core/Buffer.h>
#include <tanh/core/BufferView.h>
#include <tanh/core/Exports.h>
#include <tanh/dsp/BaseProcessor.h>
#include <tanh/dsp/filter/Butterworth.h>
#include <tanh/dsp/filter/LinkwitzRiley.h>
#include <tanh/dsp/utils/SmoothedValue.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace thl::dsp::utils {

/**
 * Splits a signal into num_bands() bands with Linkwitz-Riley crossovers in series, band 0 the
 * lowest. Each band also goes through the all-passes of the crossovers above it, so the bands sum
 * to the input through the all-passes of every crossover: their sum before processing is the dry
 * signal.
 *
 * A processor derives from it and implements process(): call process_band() for bands 0 to
 * num_bands() - 1 in this order, with the input in the buffer of band 0. Every band must be
 * processed in every block: each band reads what the band before it left behind, and its filters
 * would otherwise miss that block.
 *
 * @par Real-Time Safety
 *   process_band() and set_frequency() are real-time safe: no allocation, no locks.
 *   prepare() allocates.
 */
class TANH_API BandSplitter : public thl::dsp::BaseProcessor {
public:
    static constexpr double k_smoothing_seconds = 0.005;

    /// Until set_frequency() is called, the crossovers are log-spaced from 100 Hz to 10 kHz.
    explicit BandSplitter(size_t num_bands, size_t order);

    BandSplitter(const BandSplitter&) = delete;
    BandSplitter& operator=(const BandSplitter&) = delete;

    void prepare(const double& sample_rate,
                 const size_t& samples_per_block,
                 const size_t& num_channels) override;

    /// Sets the target of the crossover between bands @p crossover and @p crossover + 1.
    /// Call it from the audio thread, before process_band() for band 0; a change takes effect
    /// from the next block.
    /// A non-finite @p frequency is ignored. The requested frequency is kept: prepare() at another
    /// sample rate clamps it again from it, so a limit of one rate does not stick.
    void set_frequency(size_t crossover, float frequency) TANH_NONBLOCKING_FUNCTION;

    /// The target of @p crossover as applied: clamped between its neighbours and below Nyquist.
    float get_frequency(size_t crossover) const;

    size_t num_bands() const { return m_num_bands; }

    /// Clears every crossover's state and ends the glides at their targets; the next block starts
    /// with band 0 (audio thread).
    void reset() TANH_NONBLOCKING_FUNCTION override;

protected:
    /// Writes band @p band_index into @p buffer. For band 0, @p buffer holds the input on entry.
    void process_band(thl::core::BufferView buffer, size_t band_index) TANH_NONBLOCKING_FUNCTION;

private:
    struct Crossover {
        filter::LinkwitzRiley m_filter;
        std::vector<filter::Butterworth> m_band_all_passes;
    };
    using Channel = std::vector<Crossover>;

    void update_prewarped(size_t num_samples) TANH_NONBLOCKING_FUNCTION;

    std::vector<Channel> m_channels;
    thl::core::Buffer<float> m_high_passed;
    std::vector<float> m_requested;    // as set
    std::vector<float> m_frequencies;  // as applied
    std::vector<SmoothedValue> m_smoothed_frequencies;
    thl::core::Buffer<float> m_prewarped;
    float m_sample_rate = 48000.0f;
    size_t m_num_bands = 0;
    size_t m_order = 0;
    size_t m_next_band = 0;
    size_t m_num_samples = 0;
};

}  // namespace thl::dsp::utils
