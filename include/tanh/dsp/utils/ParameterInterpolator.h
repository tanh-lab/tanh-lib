#pragma once

#include <cstddef>

namespace thl::dsp::utils {

/**
 * Lightweight linear ramp generator for block-based DSP interpolation.
 *
 * `LinearRamp` emits `size` evenly spaced values from `start` towards `target`
 * when `next()` is called once per sample. It does not mutate external state.
 */
class LinearRamp {
public:
    /**
     * Create a ramp from `start` to `target` over `size` steps.
     *
     * When `size` is 0, the increment is 0 and the ramp remains at `start`.
     */
    LinearRamp(float start, float target, size_t size) noexcept
        : m_current(start),
          m_increment(size == 0 ? 0.0f : (target - start) / static_cast<float>(size)),
          m_target(target) {}

    /**
     * Return the current value and advance by one step.
     */
    float next() noexcept {
        float out = m_current;
        m_current += m_increment;
        return out;
    }

    /**
     * Return a value at a fractional offset from the current ramp position.
     *
     * @param t Fractional sample offset relative to the current position.
     */
    float at_offset(float t) const noexcept {
        return m_current + m_increment * t;
    }

    /**
     * Get the current internal ramp position.
     */
    float current_value() const noexcept {
        return m_current;
    }

    /**
     * Get the target value provided at construction time.
     */
    float final_value() const noexcept {
        return m_target;
    }

    /**
     * Get the per-step increment.
     */
    float increment() const noexcept {
        return m_increment;
    }

private:
    float m_current;
    float m_increment;
    float m_target;
};

/**
 * RAII wrapper around `LinearRamp` that commits the final ramp state on scope exit.
 *
 * This preserves the classic Mutable Instruments `ParameterInterpolator`
 * convenience pattern while exposing a reusable `LinearRamp` implementation.
 * The referenced `state` is updated in the destructor to the ramp's current
 * position (typically the next block start value after consuming the block).
 */
class ParameterInterpolator {
public:
    /**
     * Create an interpolator bound to a persistent parameter state.
     *
     * @param state In/out parameter state carried across audio blocks.
     * @param new_value Target value to approach over the current block.
     * @param size Number of samples in the block.
     */
    ParameterInterpolator(float& state, float new_value, size_t size) noexcept
        : m_state(state),
          m_ramp(state, new_value, size) {}

    /**
     * Commit the ramp position back to the referenced state.
     */
    ~ParameterInterpolator() noexcept {
        m_state = m_ramp.current_value();
    }

    ParameterInterpolator(const ParameterInterpolator&) = delete;
    ParameterInterpolator& operator=(const ParameterInterpolator&) = delete;
    ParameterInterpolator(ParameterInterpolator&&) = delete;
    ParameterInterpolator& operator=(ParameterInterpolator&&) = delete;

    /**
     * Return the current interpolated value and advance one step.
     */
    float next() noexcept {
        return m_ramp.next();
    }

    /**
     * Return a value at a fractional offset from the current interpolator position.
     */
    float at_offset(float t) const noexcept {
        return m_ramp.at_offset(t);
    }

private:
    float& m_state;
    LinearRamp m_ramp;
};

} // namespace thl::dsp::utils
