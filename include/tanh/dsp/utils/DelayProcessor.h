#pragma once

#include <tanh/core/BufferView.h>
#include <tanh/core/Exports.h>
#include <tanh/dsp/BaseProcessor.h>
#include <tanh/dsp/utils/DynamicDelayLine.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace thl::dsp::utils {

/**
 * Delays every channel by a whole number of samples, in place.
 *
 * The delay can change at runtime up to a maximum; prepare() allocates one
 * DynamicDelayLine per channel for that maximum. A delay of 0 passes the input
 * through but keeps writing the lines, so a later, longer delay starts with the
 * samples that came before it.
 *
 * Once audio has been processed, a new delay crossfades linearly from the old
 * one over crossfade_samples(). A delay set during a crossfade starts a new
 * crossfade from the delay being faded to.
 *
 * The delay is the intended output, e.g. to align a signal with a processor
 * that has latency, so the processor reports no latency itself.
 *
 * @par Real-Time Safety
 *   process(), write() and set_delay_samples() are real-time safe — no
 *   allocation, no locks. prepare() and set_max_delay_samples() allocate.
 */
class TANH_API DelayProcessor : public thl::dsp::BaseProcessor {
public:
    DelayProcessor();
    explicit DelayProcessor(size_t max_delay_samples);
    ~DelayProcessor() override;

    DelayProcessor(const DelayProcessor&) = delete;
    DelayProcessor& operator=(const DelayProcessor&) = delete;
    DelayProcessor(DelayProcessor&&) noexcept;
    DelayProcessor& operator=(DelayProcessor&&) noexcept;

    void prepare(const double& sample_rate,
                 const size_t& samples_per_block,
                 const size_t& num_channels) override;
    void process(thl::core::BufferView buffer,
                 uint32_t modulation_offset = 0) TANH_NONBLOCKING_FUNCTION override;

    /// Writes the input into the delay lines without changing it, so their history stays current.
    void write(const thl::core::BufferView& buffer) TANH_NONBLOCKING_FUNCTION;

    void reset();

    /// Reallocates and clears the delay lines once prepared. Configure outside audio processing.
    void set_max_delay_samples(size_t max_delay_samples);
    /// Clamped to max_delay_samples().
    void set_delay_samples(size_t delay_samples) TANH_NONBLOCKING_FUNCTION;
    void set_crossfade_samples(size_t crossfade_samples);

    size_t max_delay_samples() const { return m_max_delay_samples; }
    size_t delay_samples() const { return m_delay_samples; }
    size_t crossfade_samples() const { return m_crossfade_samples; }

private:
    void prepare_delay_lines();
    void advance_crossfade(size_t num_samples) TANH_NONBLOCKING_FUNCTION;

    std::vector<DynamicDelayLine> m_delay_lines;
    size_t m_num_channels = 0;
    size_t m_max_delay_samples = 0;
    size_t m_delay_samples = 0;           // faded to, or applied
    size_t m_previous_delay_samples = 0;  // faded from
    size_t m_crossfade_samples = 64;
    size_t m_crossfade_position = 0;
    bool m_prepared = false;
    bool m_has_processed = false;
};

}  // namespace thl::dsp::utils
