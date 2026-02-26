#pragma once
#include <cassert>
#include <cstddef>
#include <algorithm>

namespace thl::dsp::utils {

class SmoothedValue
{
public:
    SmoothedValue();

    enum ValueSmoothingType
    {
        Linear = 0,
    };

    void reset(double sample_rate, double time_in_seconds, ValueSmoothingType smoothing_type = Linear);

    void set_target_value(float target_value);
    void set_current_and_target_value(float target_value);
    float get_smoothed_value(std::size_t num_samples = 1);

private:
    void recalc_step();

    float m_current_value = 0.0f;
    float m_target_value = 0.0f;
    float m_step = 0.0f;

    size_t m_time_in_samples = 0;
    size_t m_samples_remaining = 0;

    double m_sample_rate = 48000.0;

    ValueSmoothingType m_smoothing_type;
};

} // namespace thl::dsp::utils


