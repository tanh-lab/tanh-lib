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
    void recalcStep();

    float m_current_value;
    float m_target_value;
    float m_step;

    size_t m_time_in_samples;
    size_t m_samples_remaining;

    double m_sample_rate;

    ValueSmoothingType m_smoothing_type;
};

} // namespace thl::dsp::utils


