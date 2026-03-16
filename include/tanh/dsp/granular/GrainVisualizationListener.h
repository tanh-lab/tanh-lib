#pragma once

namespace thl::dsp::granular {

/**
 * Optional listener interface for grain visualization.
 * All callbacks are invoked from the audio thread.
 * Implementations must be real-time safe (no allocations, no locks).
 */
class GrainVisualizationListener {
public:
    virtual ~GrainVisualizationListener() = default;

    /**
     * Called when a new grain is triggered.
     * @param grain_index  Index in the grain pool (0..MAX_GRAINS-1)
     * @param position     Normalized start position in sample (0-1)
     * @param size         Normalized grain size relative to sample length (0-1)
     * @param velocity     Playback speed factor
     * @param duration_ms  Grain duration in milliseconds
     */
    virtual void on_grain_triggered(int grain_index,
                                     float position,
                                     float size,
                                     float velocity,
                                     float duration_ms) = 0;

    /**
     * Called periodically (at a configurable rate) with current grain state.
     * @param grain_index         Index in the grain pool
     * @param normalized_position Current playhead position in sample (0-1)
     * @param envelope_amplitude  Current envelope output value (0-1)
     */
    virtual void on_grain_updated(int grain_index,
                                   float normalized_position,
                                   float envelope_amplitude) = 0;

    /**
     * Called when a grain finishes playback.
     * @param grain_index  Index in the grain pool
     */
    virtual void on_grain_finished(int grain_index) = 0;

    /**
     * Called periodically with the master ADSR envelope value for this voice.
     * @param value  Master envelope amplitude (0-1)
     */
    virtual void on_master_envelope_updated(float value) = 0;
};

}  // namespace thl::dsp::granular
