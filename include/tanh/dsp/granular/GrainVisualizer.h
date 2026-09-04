#pragma once

#include <tanh/dsp/granular/GrainVisualizationListener.h>

#include <algorithm>
#include <cstddef>
#include <vector>

namespace thl::dsp::granular {

// The voice's one outlet to the visualisation: owns the listener list and
// both reporting cadences. Grains report rate-limited (48 per voice earn a
// limit, see grain_frame_due); the Sample head reports every block in grain
// slot k_head_slot. Whoever silences a grain or the head — render, reset,
// mode switch — tells the visualiser, so nothing stale stays painted.
// Listener registration allocates and belongs to the setup thread; the
// emitters are audio-thread safe.
class GrainVisualizer {
public:
    static constexpr int k_head_slot = 0;

    void set_listener(GrainVisualizationListener* listener) {
        m_listeners.clear();
        if (listener) { m_listeners.push_back(listener); }
    }
    void add_listener(GrainVisualizationListener* listener) {
        if (listener) { m_listeners.push_back(listener); }
    }
    void remove_listener(GrainVisualizationListener* listener) {
        std::erase(m_listeners, listener);
    }
    bool has_listeners() const { return !m_listeners.empty(); }

    // Grain-path cadence. 0 fps disables grain position updates.
    void set_update_rate(double sample_rate, float fps) {
        m_update_interval =
            (fps > 0.f && sample_rate > 0) ? static_cast<size_t>(sample_rate / fps) : 0;
    }
    // Advance the grain-path clock by one block; true when an update is due.
    bool grain_frame_due(size_t num_frames) {
        if (!has_listeners() || m_update_interval == 0) { return false; }
        m_update_counter += num_frames;
        if (m_update_counter < m_update_interval) { return false; }
        m_update_counter = 0;
        return true;
    }

    // Grain events (normalised positions are fractions of the bank).
    void grain_triggered(int slot,
                         float norm_start,
                         float norm_length,
                         float velocity,
                         float duration_ms) {
        for (auto* l : m_listeners) {
            l->on_grain_triggered(slot, norm_start, norm_length, velocity, duration_ms);
        }
    }
    void grain_finished(int slot) {
        for (auto* l : m_listeners) { l->on_grain_finished(slot); }
    }
    void grain_updated(int slot, float norm_position, float envelope) {
        for (auto* l : m_listeners) { l->on_grain_updated(slot, norm_position, envelope); }
    }
    // The voice's envelope level, set per block; reported with the next
    // grain frame or head update.
    void set_master_level(float level) { m_master_level = level; }
    void report_master_level() {
        for (auto* l : m_listeners) { l->on_master_envelope_updated(m_master_level); }
    }

    // Sample head: occupies k_head_slot; reports per block, not rate-limited.
    void head_started(float norm_start, float norm_length, float velocity, float duration_ms) {
        grain_triggered(k_head_slot, norm_start, norm_length, velocity, duration_ms);
    }
    void head_finished() { grain_finished(k_head_slot); }
    void head_updated(float norm_position) {
        report_master_level();
        grain_updated(k_head_slot, norm_position, 1.0f);
    }

private:
    std::vector<GrainVisualizationListener*> m_listeners;
    size_t m_update_interval{0};  // in samples, 0 = disabled
    size_t m_update_counter{0};
    float m_master_level{0.0f};
};

}  // namespace thl::dsp::granular
