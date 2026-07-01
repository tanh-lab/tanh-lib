#pragma once

#include <cassert>
#include <cstddef>
#include <vector>

namespace thl::dsp::utils {

/**
 * Persistent linear smoothing for many independent floating-point lanes.
 *
 * The storage is contiguous and all ramp state is pre-sized, so advancing lanes
 * during audio processing does not allocate. It is intended for parameter banks
 * such as delay times, feedback matrices, gains, or modulation-control values.
 */
class LinearSmootherBank {
public:
    void resize(size_t lane_count, float initial_value = 0.0f) {
        m_current.assign(lane_count, initial_value);
        m_target.assign(lane_count, initial_value);
        m_step.assign(lane_count, 0.0f);
        m_samples_remaining.assign(lane_count, 0);
    }

    size_t lane_count() const noexcept { return m_current.size(); }
    bool empty() const noexcept { return m_current.empty(); }

    void set_ramp_samples(size_t ramp_samples) noexcept {
        m_ramp_samples = ramp_samples;
        for (size_t lane = 0; lane < m_current.size(); ++lane) {
            if (m_samples_remaining[lane] > 0) { start_ramp(lane); }
        }
    }

    size_t ramp_samples() const noexcept { return m_ramp_samples; }

    void set_current_and_target(size_t lane, float value) noexcept {
        assert(lane < m_current.size());
        m_current[lane] = value;
        m_target[lane] = value;
        m_step[lane] = 0.0f;
        m_samples_remaining[lane] = 0;
    }

    void set_target(size_t lane, float value) noexcept {
        assert(lane < m_current.size());
        if (value == m_target[lane]) { return; }

        m_target[lane] = value;
        start_ramp(lane);
    }

    void set_target(size_t lane, float value, size_t ramp_samples) noexcept {
        assert(lane < m_current.size());
        if (value == m_target[lane]) { return; }

        m_target[lane] = value;
        start_ramp(lane, ramp_samples);
    }

    void snap_to_targets() noexcept {
        for (size_t lane = 0; lane < m_current.size(); ++lane) {
            m_current[lane] = m_target[lane];
            m_step[lane] = 0.0f;
            m_samples_remaining[lane] = 0;
        }
    }

    float next(size_t lane) noexcept {
        assert(lane < m_current.size());
        if (m_samples_remaining[lane] == 0) {
            m_current[lane] = m_target[lane];
            return m_current[lane];
        }

        m_current[lane] += m_step[lane];
        --m_samples_remaining[lane];

        if (m_samples_remaining[lane] == 0) {
            m_current[lane] = m_target[lane];
            m_step[lane] = 0.0f;
        }

        return m_current[lane];
    }

    void next_all(float* output, size_t output_count) noexcept {
        assert(output_count <= m_current.size());
        for (size_t lane = 0; lane < output_count; ++lane) { output[lane] = next(lane); }
    }

    float current(size_t lane) const noexcept {
        assert(lane < m_current.size());
        return m_current[lane];
    }

    float target(size_t lane) const noexcept {
        assert(lane < m_target.size());
        return m_target[lane];
    }

    size_t samples_remaining(size_t lane) const noexcept {
        assert(lane < m_samples_remaining.size());
        return m_samples_remaining[lane];
    }

private:
    void start_ramp(size_t lane) noexcept { start_ramp(lane, m_ramp_samples); }

    void start_ramp(size_t lane, size_t ramp_samples) noexcept {
        if (ramp_samples == 0) {
            m_current[lane] = m_target[lane];
            m_step[lane] = 0.0f;
            m_samples_remaining[lane] = 0;
            return;
        }

        m_samples_remaining[lane] = ramp_samples;
        m_step[lane] =
            (m_target[lane] - m_current[lane]) / static_cast<float>(m_samples_remaining[lane]);
    }

    std::vector<float> m_current;
    std::vector<float> m_target;
    std::vector<float> m_step;
    std::vector<size_t> m_samples_remaining;
    size_t m_ramp_samples = 64;
};

}  // namespace thl::dsp::utils
