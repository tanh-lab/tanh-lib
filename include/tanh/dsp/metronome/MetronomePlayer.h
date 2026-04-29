#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include <tanh/core/Exports.h>
#include <tanh/dsp/BaseProcessor.h>
#include <tanh/dsp/transport/TransportClock.h>
#include <tanh/dsp/utils/ADSR.h>

namespace thl::dsp::metronome {

/**
 * @class MetronomePlayerImpl
 * @brief Sample-accurate metronome processor driven by a TransportClock.
 *
 * Mixes its output into the buffer (additive), so it can be placed anywhere
 * in the audio processing chain without silencing upstream signal.
 * Uses a voice pool with ADSR-shaped decaying sines for click sounds.
 *
 * Two sounds:
 *   Accent — bar downbeat:  800 Hz, 20 ms exponential decay
 *   Click  — beat/division: 1200 Hz, 12 ms exponential decay
 *
 * Override trigger_accent(), trigger_click(), and tick_voices() to replace the
 * built-in sine clicks with custom sounds (e.g. sample playback).
 *
 * @section lifetime Lifetime
 *   The TransportClock reference passed in must outlive this player.
 *
 * @section modulation Modulation
 *   The Rhythm parameter is sampled once per process() block; sub-block
 *   change points are not honored. Gain and Enabled may modulate per-trigger.
 *
 * @section rt_safety Real-Time Safety
 *   process() is real-time safe — no allocation, no locks.
 */
class TANH_API MetronomePlayerImpl : public BaseProcessor {
public:
    explicit MetronomePlayerImpl(transport::TransportClock& clock);

    void prepare(const double& sample_rate,
                 const size_t& samples_per_block,
                 const size_t& num_channels) override;

    void process(thl::dsp::audio::AudioBufferView buffer, uint32_t modulation_offset = 0) override;

protected:
    // ── Parameters ───────────────────────────────────────────────────────────
    // Concrete implementations must provide these via the State system.

    enum Parameter {
        Enabled,  ///< bool — metronome on/off
        Gain,     ///< float — master volume (0.0–1.0)
        Rhythm,   ///< int — subdivision (0=Bar, 1=Half, 2=Beat, 3=Eighth, 4=Sixteenth)
        NumParameters
    };

    // ── Override points ──────────────────────────────────────────────────────
    // Override these to replace the built-in sine clicks with custom sounds
    // (e.g. sample playback). The base implementations use the voice pool.

    /// Called at the exact sample where a bar boundary falls.
    virtual void trigger_accent();

    /// Called at the exact sample where a rhythm boundary falls.
    virtual void trigger_click();

    /// Render one sample from all active voices. Called once per sample.
    virtual float tick_voices();

    // ── Voice pool (used by default implementations) ─────────────────────────

    struct Voice {
        thl::dsp::utils::ADSR m_env;
        float m_phase = 0.0f;
        float m_freq = 1000.0f;
        float m_gain = 1.0f;

        void trigger(float sample_rate, float in_freq, float in_gain, float release_ms);
        float tick(float sample_rate);
    };

    Voice& pick_voice();

    /// Number of voices in the pool. Each trigger claims the quietest voice,
    /// so overlapping clicks fade out naturally without phase discontinuities.
    /// 4 is generous headroom — at most 2 clicks overlap in practice, but 4
    /// keeps us safe if decay times are extended or divisions are very fast.
    static constexpr int k_num_voices = 4;
    std::array<Voice, k_num_voices> m_voices;

    static constexpr float k_attack_ms = 1.5f;
    static constexpr float k_accent_freq = 800.0f;
    static constexpr float k_accent_decay_ms = 20.0f;
    static constexpr float k_click_freq = 1200.0f;
    static constexpr float k_click_decay_ms = 12.0f;

    /// Accent gain relative to master. Bar downbeat is louder than subdivision.
    static constexpr float k_accent_gain_factor = 1.0f;
    static constexpr float k_click_gain_factor = 0.6f;

private:
    // Template wrapper for get_parameter
    template <typename T>
    T get_parameter(Parameter parameter, uint32_t modulation_offset = 0);

    virtual float get_parameter_float(Parameter parameter, uint32_t modulation_offset = 0) = 0;
    virtual bool get_parameter_bool(Parameter parameter, uint32_t modulation_offset = 0) = 0;
    virtual int get_parameter_int(Parameter parameter, uint32_t modulation_offset = 0) = 0;

    [[nodiscard]] std::optional<uint32_t> crossing_offset(double division_beats,
                                                          double beat_start,
                                                          double bps,
                                                          uint32_t frame_count) const;

    transport::TransportClock& m_clock;
    double m_sample_rate = 48000.0;
};

// Template specializations for get_parameter
template <>
inline float MetronomePlayerImpl::get_parameter<float>(Parameter p, uint32_t modulation_offset) {
    return get_parameter_float(p, modulation_offset);
}
template <>
inline bool MetronomePlayerImpl::get_parameter<bool>(Parameter p, uint32_t modulation_offset) {
    return get_parameter_bool(p, modulation_offset);
}
template <>
inline int MetronomePlayerImpl::get_parameter<int>(Parameter p, uint32_t modulation_offset) {
    return get_parameter_int(p, modulation_offset);
}

}  // namespace thl::dsp::metronome
