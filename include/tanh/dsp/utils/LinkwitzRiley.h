#pragma once

#include <tanh/dsp/utils/Butterworth.h>

#include <cassert>
#include <cstddef>

namespace thl::dsp::utils {

/**
 * Linkwitz-Riley crossover of even order 2 to k_max_order: each band is a Butterworth of half the
 * order, run twice, so both bands are −6 dB at the cutoff and sum to an all-pass. For orders 2 and
 * 6 the high band is inverted, so low + high is the all-pass for every order.
 *
 * Call prepare() before use.
 */
class LinkwitzRiley {
public:
    static constexpr size_t k_max_order = 2 * Butterworth::k_max_order;

    LinkwitzRiley() = default;

    void prepare(float frequency, float sample_rate, size_t order) {
        assert(order >= 2 && order <= k_max_order && order % 2 == 0);
        const size_t butterworth_order = order / 2;
        m_low_1.prepare(frequency, sample_rate, butterworth_order, FilterType::LowPass);
        m_low_2.prepare(frequency, sample_rate, butterworth_order, FilterType::LowPass);
        m_high_1.prepare(frequency, sample_rate, butterworth_order, FilterType::HighPass);
        m_high_2.prepare(frequency, sample_rate, butterworth_order, FilterType::HighPass);
        m_invert_high = butterworth_order % 2 == 1;
    }

    void set_frequency(float frequency) {
        m_low_1.set_frequency(frequency);
        m_low_2.set_frequency(frequency);
        m_high_1.set_frequency(frequency);
        m_high_2.set_frequency(frequency);
    }

    void reset() {
        m_low_1.reset();
        m_low_2.reset();
        m_high_1.reset();
        m_high_2.reset();
    }

    float process_low(float x) { return m_low_2.process(m_low_1.process(x)); }

    float process_high(float x) {
        const float high = m_high_2.process(m_high_1.process(x));
        return m_invert_high ? -high : high;
    }

    // With g from prewarp() instead of the frequency of set_frequency(), as Butterworth::process().
    float process_low(float x, float g) { return m_low_2.process(m_low_1.process(x, g), g); }

    float process_high(float x, float g) {
        const float high = m_high_2.process(m_high_1.process(x, g), g);
        return m_invert_high ? -high : high;
    }

    void process(float x, float& low, float& high) {
        low = process_low(x);
        high = process_high(x);
    }

    LinkwitzRiley(const LinkwitzRiley&) = delete;
    LinkwitzRiley& operator=(const LinkwitzRiley&) = delete;

private:
    Butterworth m_low_1;
    Butterworth m_low_2;
    Butterworth m_high_1;
    Butterworth m_high_2;
    bool m_invert_high = false;
};

}  // namespace thl::dsp::utils
