#include <tanh/dsp/utils/ADSR.h>
#include <algorithm>  // for std::max and std::min
#include <cmath>      // for calculations

namespace thl::dsp::utils {

ADSR::ADSR()
    : m_attack_time(10.0f)
    , m_decay_time(100.0f)
    , m_sustain_level(0.7f)
    , m_release_time(500.0f)
    , m_attack_curve(0.0f)
    ,  // Default to linear curves
    m_decay_curve(0.0f)
    , m_release_curve(0.0f)
    , m_state(State::IDLE)
    , m_current_level(0.0f)
    , m_attack_rate(0.0f)
    , m_decay_rate(0.0f)
    , m_release_rate(0.0f)
    , m_release_level(0.0f)
    , m_sample_rate(44100.0f) {
    // Calculate initial rates
    update_rates();
}

void ADSR::set_parameters(float attack_time,
                          float decay_time,
                          float sustain_level,
                          float release_time) {
    m_attack_time = std::max(0.1f, attack_time);
    m_decay_time = std::max(0.1f, decay_time);
    m_sustain_level = std::clamp(sustain_level, 0.0f, 1.0f);
    m_release_time = std::max(0.1f, release_time);

    update_rates();
}

void ADSR::set_parameters(float attack_time,
                          float decay_time,
                          float sustain_level,
                          float release_time,
                          float attack_curve,
                          float decay_curve,
                          float release_curve) {
    // Set timing parameters
    m_attack_time = std::max(0.1f, attack_time);
    m_decay_time = std::max(0.1f, decay_time);
    m_sustain_level = std::clamp(sustain_level, 0.0f, 1.0f);
    m_release_time = std::max(0.1f, release_time);

    // Set curve blend parameters
    // Range: -1.0 = inverse exponential, 0.0 = linear, 1.0 = exponential
    m_attack_curve = std::clamp(attack_curve, -1.0f, 1.0f);
    m_decay_curve = std::clamp(decay_curve, -1.0f, 1.0f);
    m_release_curve = std::clamp(release_curve, -1.0f, 1.0f);

    update_rates();
}

void ADSR::set_attack(float time_ms) {
    m_attack_time = std::max(0.1f, time_ms);
    update_rates();
}

void ADSR::set_decay(float time_ms) {
    m_decay_time = std::max(0.1f, time_ms);
    update_rates();
}

void ADSR::set_sustain(float level) {
    m_sustain_level = std::clamp(level, 0.0f, 1.0f);
}

void ADSR::set_release(float time_ms) {
    m_release_time = std::max(0.1f, time_ms);
    update_rates();
}

void ADSR::set_sample_rate(float sample_rate) {
    m_sample_rate = std::max(1.0f, sample_rate);
    update_rates();
}

void ADSR::note_on() {
    m_state = State::ATTACK;
    // If already playing, don't reset the level - allows for legato
}

void ADSR::note_off() {
    // Only move to release phase if we're currently active in a sustaining
    // phase
    if (m_state != State::IDLE) {
        m_state = State::RELEASE;
        m_release_level = m_current_level;  // Set the level at which release
                                            // began
    }
}

void ADSR::reset() {
    m_state = State::IDLE;
    m_current_level = 0.0f;
}

float ADSR::process() {
    float exponential_normalization = 1.0f - std::exp(-5.f);
    switch (m_state) {
        case State::IDLE: break;

        case State::ATTACK:
            // Apply blended curve for attack phase
            // Positive: fast start, slow finish. Negative: slow start, fast finish.
            if (std::abs(m_attack_curve) > 0.01f) {
                float abs_curve = std::abs(m_attack_curve);

                // Calculate exponential curve
                float exponential_target =
                    (1.00f - std::exp(-(m_attack_rate * 5.0f))) / exponential_normalization;
                float linear_increment = m_attack_rate;

                // Positive: weight by distance to target (1 - level)
                // Negative: weight by distance from start (level), bowing the other way
                float weight =
                    m_attack_curve > 0.0f ? (1.0f - m_current_level) : (m_current_level + 0.01f);

                // Apply blended increment between linear and exponential
                float blended_increment =
                    (1.0f - abs_curve) * linear_increment + abs_curve * exponential_target * weight;
                m_current_level += blended_increment;
            } else {
                // Simple linear attack (original behavior)
                m_current_level += m_attack_rate;
            }

            // If we've reached the peak, move to decay phase
            if (m_current_level >= 1.0f - 1e-2f) {  // For exponential curves,
                                                    // allow a small tolerance
                m_current_level = 1.0f;
                m_state = State::DECAY;
            }
            break;

        case State::DECAY:
            // Apply blended curve for decay phase
            // Positive: fast drop, slow tail. Negative: slow drop, fast tail.
            if (std::abs(m_decay_curve) > 0.01f) {
                float abs_curve = std::abs(m_decay_curve);

                // Calculate the target level and distance from current to
                // target
                float target_level = m_sustain_level;
                float distance = m_current_level - target_level;
                float total_range = std::max(1.0f - target_level, 0.001f);

                // Calculate exponential decay factor
                float exp_factor =
                    (1.0f - std::exp(-(m_decay_rate * 5.0f))) / exponential_normalization;
                float linear_decay = m_decay_rate;

                // Positive: weight by distance (fast start). Negative: weight by progress (fast
                // end).
                float weight = m_decay_curve > 0.0f
                                   ? distance
                                   : total_range * (1.0f - distance / total_range + 0.01f);

                // Apply blended decrement between linear and exponential
                float blended_decrement =
                    (1.0f - abs_curve) * linear_decay + abs_curve * exp_factor * weight;
                m_current_level -= blended_decrement;
            } else {
                // Simple linear decay (original behavior)
                m_current_level -= m_decay_rate;
            }

            // If we've reached sustain level, move to sustain phase
            if (m_current_level <= m_sustain_level + 1e-2f) {
                m_current_level = m_sustain_level;  // Avoid floating point
                                                    // issues
                m_state = State::SUSTAIN;
            }
            break;

        case State::SUSTAIN:
            // In sustain phase, level stays constant at sustain_level
            m_current_level = m_sustain_level;  // Avoid floating point issues
            break;

        case State::RELEASE:
            // Apply blended curve for release phase
            // Positive: fast drop, slow tail. Negative: slow drop, fast tail.
            if (std::abs(m_release_curve) > 0.01f) {
                float abs_curve = std::abs(m_release_curve);
                float safe_release_level = std::max(m_release_level, 0.001f);

                // Calculate exponential decay factor
                float exp_factor =
                    (1.0f - std::exp(-(m_release_rate * 5.0f))) / exponential_normalization;
                float linear_decay = m_release_rate;

                // Positive: weight by current level (fast start). Negative: weight by progress
                // (fast end).
                float weight = m_release_curve > 0.0f
                                   ? m_current_level
                                   : safe_release_level *
                                         (1.0f - m_current_level / safe_release_level + 0.01f);

                // Apply blended decrement between linear and exponential
                float blended_decrement =
                    (1.0f - abs_curve) * linear_decay + abs_curve * exp_factor * weight;
                m_current_level -= blended_decrement;
            } else {
                // Simple linear release (original behavior)
                m_current_level -= m_release_rate;
            }

            // If we've reached zero, move to idle phase
            if (m_current_level <= 1e-7f) {
                m_current_level = 0.0f;
                m_state = State::IDLE;
            }
            break;
    }

    // Ensure the level stays within valid range
    m_current_level = std::clamp(m_current_level, 0.0f, 1.0f);

    return m_current_level;
}

bool ADSR::is_active() const {
    return m_state != State::IDLE;
}

ADSR::State ADSR::get_state() const {
    return m_state;
}

void ADSR::set_attack_curve(float curve_blend) {
    m_attack_curve = std::clamp(curve_blend, -1.0f, 1.0f);
}

void ADSR::set_decay_curve(float curve_blend) {
    m_decay_curve = std::clamp(curve_blend, -1.0f, 1.0f);
}

void ADSR::set_release_curve(float curve_blend) {
    m_release_curve = std::clamp(curve_blend, -1.0f, 1.0f);
}

void ADSR::set_curves(float attack_curve, float decay_curve, float release_curve) {
    m_attack_curve = std::clamp(attack_curve, -1.0f, 1.0f);
    m_decay_curve = std::clamp(decay_curve, -1.0f, 1.0f);
    m_release_curve = std::clamp(release_curve, -1.0f, 1.0f);
}

void ADSR::update_rates() {
    // Calculate attack rate per sample
    if (m_attack_time > 0.0f) {
        // Convert ms to seconds and calculate samples
        float attack_samples = (m_attack_time / 1000.0f) * m_sample_rate;
        m_attack_rate = 1.0f / attack_samples;
    } else {
        m_attack_rate = 1.0f;  // Instant attack
    }

    // Calculate decay rate per sample
    if (m_decay_time > 0.0f) {
        float decay_samples = (m_decay_time / 1000.0f) * m_sample_rate;
        m_decay_rate = (1.0f - m_sustain_level) / decay_samples;
    } else {
        m_decay_rate = 1.0f;  // Instant decay
    }

    // Calculate release rate per sample
    if (m_release_time > 0.0f) {
        float release_samples = (m_release_time / 1000.0f) * m_sample_rate;
        m_release_rate = 1.0f / release_samples;
    } else {
        m_release_rate = 1.0f;  // Instant release
    }
}

}  // namespace thl::dsp::utils
