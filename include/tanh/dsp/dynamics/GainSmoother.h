#pragma once

#include <tanh/core/AbiNamespace.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace thl::inline THL_ABI_NAMESPACE::dsp::dynamics {

/**
 * @brief The ballistics of a dynamics processor: smooths its gain in dB.
 *
 * Smoothing the computed gain, not the level (the "smooth decoupled" detector of Giannoulis,
 * Massberg and Reiss 2012), gives the attack and release their own times whatever the threshold
 * and ratio. The gain moves with the attack time while its target lies farther from 0 dB than
 * it (more gain change: in compression, out of an upward mode), with the release time otherwise;
 * both are one-pole time constants (the step reaches 1 - 1/e, 63 %, after one).
 *
 * - Hold: a release waits this long after the gain last moved away from 0 dB or held at its
 *   target (a sustained compression holds from its end).
 * - Auto release: a second, slow stage (10× the attack, 10× the release) follows the same target,
 *   and the gain is whichever stage lies farther from 0 dB. A short peak hardly moves the slow
 *   stage and releases at the release time; a long compression builds it up and releases slowly,
 *   which pumps less.
 *
 * Threads: set_times() computes coefficients (std::exp); all is real-time safe.
 */
class GainSmoother {
public:
    static constexpr float k_auto_release_factor = 10.0f;

    void prepare(double sample_rate) {
        m_sample_rate = sample_rate;
        update_coefficients();
        reset();
    }

    /// Attack, release and hold in ms; at 0 the gain jumps (attack, release) or does not wait.
    void set_times(float attack_ms,
                   float release_ms,
                   float hold_ms = 0.0f,
                   bool auto_release = false) TANH_NONBLOCKING_FUNCTION {
        m_auto_release = auto_release;
        if (attack_ms == m_attack_ms && release_ms == m_release_ms && hold_ms == m_hold_ms) {
            return;
        }
        m_attack_ms = attack_ms;
        m_release_ms = release_ms;
        m_hold_ms = hold_ms;
        update_coefficients();
    }

    /// Jumps to @p gain_db, both stages, the hold over.
    void reset(float gain_db = 0.0f) TANH_NONBLOCKING_FUNCTION {
        m_fast = {.m_gain_db = gain_db, .m_hold_remaining = 0};
        m_slow = {.m_gain_db = gain_db, .m_hold_remaining = 0};
    }

    /// One sample towards @p target_db; the smoothed gain in dB.
    [[nodiscard]] float process(float target_db) TANH_NONBLOCKING_FUNCTION {
        step(m_fast, target_db, m_attack, m_release);
        if (!m_auto_release) { return m_fast.m_gain_db; }
        step(m_slow, target_db, m_slow_attack, m_slow_release);
        return std::abs(m_slow.m_gain_db) > std::abs(m_fast.m_gain_db) ? m_slow.m_gain_db
                                                                       : m_fast.m_gain_db;
    }

    /// The gain in dB as last returned (without auto release: the fast stage).
    [[nodiscard]] float gain_db() const TANH_NONBLOCKING_FUNCTION {
        if (!m_auto_release) { return m_fast.m_gain_db; }
        return std::abs(m_slow.m_gain_db) > std::abs(m_fast.m_gain_db) ? m_slow.m_gain_db
                                                                       : m_fast.m_gain_db;
    }

private:
    struct Stage {
        float m_gain_db = 0.0f;
        uint32_t m_hold_remaining = 0;
    };

    void step(Stage& stage,
              float target_db,
              float attack,
              float release) const TANH_NONBLOCKING_FUNCTION {
        // At its target counts as attack: the hold runs from the end of the gain change.
        if (std::abs(target_db) >= std::abs(stage.m_gain_db)) {
            stage.m_gain_db = target_db + (attack * (stage.m_gain_db - target_db));
            stage.m_hold_remaining = m_hold_samples;
        } else if (stage.m_hold_remaining > 0) {
            --stage.m_hold_remaining;
        } else {
            stage.m_gain_db = target_db + (release * (stage.m_gain_db - target_db));
        }
    }

    /// exp(-1 / (time · fs)): 0 (a jump) for no time.
    [[nodiscard]] float coefficient(float time_ms) const TANH_NONBLOCKING_FUNCTION {
        const double samples = static_cast<double>(time_ms) * 0.001 * m_sample_rate;
        return samples > 0.0 ? static_cast<float>(std::exp(-1.0 / samples)) : 0.0f;
    }

    void update_coefficients() TANH_NONBLOCKING_FUNCTION {
        m_attack = coefficient(m_attack_ms);
        m_release = coefficient(m_release_ms);
        m_slow_attack = coefficient(k_auto_release_factor * m_attack_ms);
        m_slow_release = coefficient(k_auto_release_factor * m_release_ms);
        m_hold_samples = static_cast<uint32_t>(
            std::lround(std::max(0.0, static_cast<double>(m_hold_ms) * 0.001 * m_sample_rate)));
    }

    double m_sample_rate = 48000.0;
    float m_attack_ms = 10.0f;
    float m_release_ms = 100.0f;
    float m_hold_ms = 0.0f;
    bool m_auto_release = false;
    float m_attack = 0.0f;
    float m_release = 0.0f;
    float m_slow_attack = 0.0f;
    float m_slow_release = 0.0f;
    uint32_t m_hold_samples = 0;
    Stage m_fast;
    Stage m_slow;
};

}  // namespace thl::inline THL_ABI_NAMESPACE::dsp::dynamics
