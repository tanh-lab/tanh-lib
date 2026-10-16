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
 * samples that came before it. A new delay takes effect at the next sample,
 * without a transition.
 *
 * The delay is the intended output, e.g. to align a signal with a processor
 * that has latency, so the processor reports no latency itself.
 *
 * @par Real-Time Safety
 *   process() and set_delay_samples() are real-time safe — no allocation, no
 *   locks. prepare() and set_max_delay_samples() allocate.
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

    void reset();

    /// Reallocates and clears the delay lines once prepared. Configure outside audio processing.
    void set_max_delay_samples(size_t max_delay_samples);
    /// Clamped to max_delay_samples().
    void set_delay_samples(size_t delay_samples);

    size_t max_delay_samples() const { return m_max_delay_samples; }
    size_t delay_samples() const { return m_delay_samples; }

private:
    void prepare_delay_lines();

    std::vector<DynamicDelayLine> m_delay_lines;
    size_t m_num_channels = 0;
    size_t m_max_delay_samples = 0;
    size_t m_delay_samples = 0;
    bool m_prepared = false;
};

}  // namespace thl::dsp::utils
