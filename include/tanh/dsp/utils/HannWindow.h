#pragma once

#include <cmath>

namespace thl::dsp::utils {

/**
 * HannWindow envelope generator class.
 * 
 * This class implements a Hann (raised cosine) window function for granular synthesis.
 * It provides a smooth bell-shaped envelope that is particularly well-suited for grain envelopes
 * in granular synthesis as it eliminates discontinuities at the grain boundaries.
 */
class HannWindow {
public:
    /**
     * HannWindow envelope state
     */
    enum class State {
        IDLE,   // Not active
        ACTIVE  // Active/running
    };

public:
    HannWindow();
    
    /**
     * Set the total window duration
     * 
     * @param duration_ms Duration of the window in milliseconds
     */
    void set_duration(float duration_ms);
    
    /**
     * Set sample rate for timing calculations
     * 
     * @param sample_rate The sample rate in Hz
     */
    void set_sample_rate(float sample_rate);
    
    /**
     * Start the envelope
     */
    void start();
    
    /**
     * Reset the envelope to IDLE state
     */
    void reset();
    
    /**
     * Process a single sample and return the envelope value at the current position
     * 
     * @return The current envelope amplitude (0.0-1.0)
     */
    float process();
    
    /**
     * Process a sample at a specific normalized position in the window
     * 
     * @param position Normalized position in the window (0.0-1.0)
     * @return The envelope amplitude at the given position (0.0-1.0)
     */
    float process_at_position(float position);
    
    /**
     * Check if the envelope is active
     * 
     * @return True if the envelope is active
     */
    bool is_active() const;

    /**
     * Get the current state of the envelope
     * 
     * @return The current state
     */
    State get_state() const;

private:
    State m_state;
    float m_sample_rate;
    float m_duration_samples;
    float m_current_sample;
};

} // namespace thl::dsp::utils
