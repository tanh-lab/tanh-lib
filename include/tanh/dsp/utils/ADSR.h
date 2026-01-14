#pragma once

namespace thl::dsp::utils {

/**
 * ADSR envelope generator class.
 * 
 * This class implements a classic ADSR (Attack, Decay, Sustain, Release)
 * envelope for audio signal processing. It can be used to shape the amplitude
 * of audio signals over time, particularly useful for synthesizers and 
 * granular processing.
 */
class ADSR {
public:
    /**
     * ADSR envelope state
     */
    enum class State {
        IDLE,       // Not playing
        ATTACK,     // Attack phase
        DECAY,      // Decay phase
        SUSTAIN,    // Sustain phase
        RELEASE     // Release phase
    };

public:
    ADSR();
    
    /**
     * Set all ADSR parameters at once
     * 
     * @param attack_time Attack time in milliseconds
     * @param decay_time Decay time in milliseconds
     * @param sustain_level Sustain level (0.0-1.0)
     * @param release_time Release time in milliseconds
     */
    void set_parameters(float attack_time, float decay_time, float sustain_level, float release_time);
    
    /**
     * Set all ADSR parameters including curve shapes at once
     * 
     * @param attack_time Attack time in milliseconds
     * @param decay_time Decay time in milliseconds
     * @param sustain_level Sustain level (0.0-1.0)
     * @param release_time Release time in milliseconds
     * @param attack_curve Attack curve blend (0.0 = linear, 1.0 = exponential)
     * @param decay_curve Decay curve blend (0.0 = linear, 1.0 = exponential)
     * @param release_curve Release curve blend (0.0 = linear, 1.0 = exponential)
     */
    void set_parameters(float attack_time, float decay_time, float sustain_level, float release_time,
                        float attack_curve, float decay_curve, float release_curve);
    
    /**
     * Set attack time
     * 
     * @param time_ms Attack time in milliseconds
     */
    void set_attack(float time_ms);
    
    /**
     * Set decay time
     * 
     * @param time_ms Decay time in milliseconds
     */
    void set_decay(float time_ms);
    
    /**
     * Set sustain level
     * 
     * @param level Sustain level (0.0-1.0)
     */
    void set_sustain(float level);
    
    /**
     * Set release time
     * 
     * @param time_ms Release time in milliseconds
     */
    void set_release(float time_ms);
    
    /**
     * Set sample rate for timing calculations
     * 
     * @param sample_rate The sample rate in Hz
     */
    void set_sample_rate(float sample_rate);
    
    /**
     * Start the envelope
     */
    void note_on();
    
    /**
     * End the envelope (begin release phase)
     */
    void note_off();
    
    /**
     * Reset the envelope to IDLE state
     */
    void reset();
    
    /**
     * Process a single sample and return the envelope value
     * 
     * @return The current envelope amplitude (0.0-1.0)
     */
    float process();
    
    /**
     * Check if the envelope is active
     * 
     * @return True if the envelope is in any stage except IDLE
     */
    bool is_active() const;
    
    /**
     * Get the current state of the envelope
     * 
     * @return The current state (IDLE, ATTACK, DECAY, SUSTAIN, RELEASE)
     */
    State get_state() const;
    
    /**
     * Set the attack curve shape
     * 
     * @param curve_blend Blend factor between linear (0.0) and exponential (1.0)
     */
    void set_attack_curve(float curve_blend);
    
    /**
     * Set the decay curve shape
     * 
     * @param curve_blend Blend factor between linear (0.0) and exponential (1.0)
     */
    void set_decay_curve(float curve_blend);
    
    /**
     * Set the release curve shape
     * 
     * @param curve_blend Blend factor between linear (0.0) and exponential (1.0)
     */
    void set_release_curve(float curve_blend);
    
    /**
     * Set all curve shapes at once
     * 
     * @param attack_curve Attack curve blend (0.0 = linear, 1.0 = exponential)
     * @param decay_curve Decay curve blend (0.0 = linear, 1.0 = exponential)
     * @param release_curve Release curve blend (0.0 = linear, 1.0 = exponential)
     */
    void set_curves(float attack_curve, float decay_curve, float release_curve);
    
private:
    // Update rates based on current parameters
    void update_rates();

    // Parameters
    float m_attack_time;   // Attack time in milliseconds
    float m_decay_time;    // Decay time in milliseconds 
    float m_sustain_level; // Sustain level (0.0-1.0)
    float m_release_time;  // Release time in milliseconds
    
    // Curve blending parameters (0.0 = linear, 1.0 = exponential)
    float m_attack_curve;  // Attack curve shape (0.0-1.0)
    float m_decay_curve;   // Decay curve shape (0.0-1.0)
    float m_release_curve; // Release curve shape (0.0-1.0)
    
    // Runtime state
    State m_state;          // Current envelope state
    float m_current_level;  // Current output level (0.0-1.0)
    float m_attack_rate;    // Rate of increase during attack phase
    float m_decay_rate;     // Rate of decrease during decay phase
    float m_release_rate;   // Rate of decrease during release phase
    float m_release_level;  // Level at which release phase began
    float m_sample_rate;    // Sample rate for timing calculations
};

} // namespace thl::dsp::utils
