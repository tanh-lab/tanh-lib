#pragma once
#include <cassert>
#include <cstddef>
#include <algorithm>

namespace thl {
namespace dsp {

class SmoothedValue
{
public:
    SmoothedValue()
        : m_current_value(0.0f),
          m_target_value(0.0f),
          m_step(0.0f),
          m_time_in_samples(0),
          m_samples_remaining(0),
          m_sample_rate(48000.0),
          m_smoothing_type(Linear)
    {
    }

    enum ValueSmoothingType
    {
        Linear = 0,
    };

    void reset(double sample_rate, double time_in_seconds, ValueSmoothingType smoothing_type = Linear) {
        assert(time_in_seconds > 0.0);
        assert(sample_rate > 0.0);

        m_sample_rate = sample_rate;
        m_smoothing_type = smoothing_type;

        m_time_in_samples = static_cast<size_t>(time_in_seconds * sample_rate);
        if (m_time_in_samples == 0) {
            m_time_in_samples = 1;
        }

        m_samples_remaining = 0;
        m_step = 0.0f;

        recalcStep();
    }

    void set_target_value(float target_value) {
        m_target_value = target_value;
        m_samples_remaining = m_time_in_samples;

        recalcStep();
    }

    void set_current_and_target_value(float target_value) {
        m_current_value = target_value;
        m_target_value = target_value;
        m_step = 0.0f;
        m_samples_remaining = 0;
    }

    float get_smoothed_value(std::size_t num_samples = 1)
    {
        if (num_samples == 0) return m_current_value;

        if (m_samples_remaining == 0)
        {
            m_current_value = m_target_value;
            return m_current_value;
        }

        std::size_t steps_to_apply = std::min(num_samples, m_samples_remaining);

        if (m_smoothing_type == Linear)
        {
            m_current_value += m_step * static_cast<float>(steps_to_apply);
        }

        m_samples_remaining -= steps_to_apply;

        if (m_samples_remaining == 0) m_current_value = m_target_value;

        return m_current_value;
    }

private:
    void recalcStep()
    {
        if (m_time_in_samples == 0 || m_samples_remaining == 0)
        {
            m_step = 0.0f;
            return;
        }

        if (m_smoothing_type == Linear)
        {
            const float distance = m_target_value - m_current_value;
            m_step = distance / static_cast<float>(m_samples_remaining);
        }
    }

    float m_current_value;
    float m_target_value;
    float m_step;

    size_t m_time_in_samples;
    size_t m_samples_remaining;

    double m_sample_rate;

    ValueSmoothingType m_smoothing_type;
};


} // namespace dsp
} // namespace thl
