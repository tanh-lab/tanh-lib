#pragma once

#include <tanh/dsp/utils/HannWindow.h>

#include <array>
#include <cstddef>

// Shared constants and enums of the granular voice. Every component of the
// voice (GrainEngine, SamplePlayer, HeadPolicy, ChannelMixer) speaks these;
// GrainProcessor.h re-exports them for the public facade.
namespace thl::dsp::granular {

// Maximum number of output channels supported by the granular voice.
constexpr size_t k_max_channel_support = 16;

// Grain size limits in seconds (will be converted to samples based on sample
// rate)
constexpr float k_min_grain_size = 0.002f;  // 2 ms
constexpr float k_max_grain_size = 0.4f;    // 400 ms

// Grain trigger rate limits in grains per second (for density control).
// Density maps exponentially between these so the control feels even.
constexpr float k_min_grain_rate = 2.0f;
constexpr float k_max_grain_rate = 100.0f;

// Maximum number of grains that can be active simultaneously.
// Must cover the worst-case overlap: k_max_grain_rate * k_max_grain_size.
constexpr size_t k_max_grains = 48;

// Duration in seconds over which temperature ramps up from 0 to full at
// playback start
constexpr float k_temperature_ramp_duration = 1.0f;

enum class ChannelMode : int {
    MonoToStereo,      // Read ch0 from source, spread across L/R
    TrueStereo,        // Read ch0+ch1 from source (mono duplicated if source is mono)
    TrueMultichannel,  // Read all source channels, write to matching output
                       // channels
    NumChannelModes
};

// Where the read head goes. All three modes share the same source buffer,
// pitch bank, ADSR and channel handling; they differ in the head policy.
enum class EngineMode : int {
    GranularPosition,  // Grains sprayed around a fixed Position (no travelling head)
    GranularLoop,      // Scan head runs Start -> End at 1x, restarts at Loop
    Sample,            // One continuous interpolating head at Velocity, no grains
    NumEngineModes
};

// Length of the equal-power crossfade the Sample head runs at a loop wrap,
// a pitch-bank switch or a retrigger (seconds).
constexpr float k_player_crossfade_duration = 0.010f;
// Fade-through-zero when the engine mode changes on a sounding voice.
constexpr float k_mode_change_fade_duration = 0.015f;

// The planar output block a render call fills. `m_num_channels` is what the
// host actually handed over this block (a device switch can deliver fewer
// than prepare() promised); pointers beyond it are null.
struct AudioBlock {
    std::array<float*, k_max_channel_support> m_channels{};
    size_t m_num_channels{0};
    size_t m_num_frames{0};
};

// Structure to represent a single grain
struct Grain {
    size_t m_start_position;    // Starting position in the sample
    size_t m_current_position;  // Current position within the grain
    size_t m_grain_size;        // Size of the grain in samples
    float m_velocity;           // Playback speed/velocity
    float m_amplitude;          // Grain amplitude/volume
    float m_gain;
    float m_position_spread;                 // Pan position [0, 1] for MonoToStereo spread
    bool m_active;                           // Whether the grain is currently active
    thl::dsp::utils::HannWindow m_envelope;  // Hann window envelope for amplitude
                                             // modulation
    size_t m_sample_index;                   // Index of the sample in the audio data
};

}  // namespace thl::dsp::granular
