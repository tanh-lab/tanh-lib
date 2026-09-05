#pragma once

#include <tanh/core/Numbers.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

// Shared constants and enums of the granular voice. Every component of the
// voice (GrainEngine, SamplePlayer, HeadPolicy, ChannelMixer) speaks these;
// GrainProcessor.h re-exports them for the public facade.
namespace thl::dsp::granular {

// Signed frame position / offset in a bank. 64-bit everywhere (`long` is
// 32-bit on Windows); unsigned bank lengths are size_t.
using FramePos = std::int64_t;

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

// Grain pitch (Velocity in the granular modes) after jitter never drops
// below this: zero or negative would produce empty grains.
constexpr float k_min_grain_pitch = 0.01f;
// Sample-mode varispeed bounds: forward and finite under modulation.
constexpr float k_min_varispeed = 0.01f;
constexpr float k_max_varispeed = 8.0f;

// Temperature response curves (exponents on [0, 1] temperature).
constexpr float k_size_temperature_curve = 3.0f;
constexpr float k_velocity_temperature_curve = 15.0f;
// Upward size jitter is damped so small grains don't balloon.
constexpr float k_size_jitter_upper_scale = 0.3f;
// Velocity jitter reaches at most a perfect fifth: 2^(7/12).
constexpr float k_velocity_jitter_ratio = 1.4983070768766815f;
// Per-grain L/R energy compensation for the TrueStereo / multichannel pan.
constexpr float k_stereo_energy_compensation = 2.0f;

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

// One grain of the pool.
struct Grain {
    size_t m_start_position{0};     // Start in the bank, frames
    size_t m_current_position{0};   // Frames rendered so far
    size_t m_grain_size{1};         // Length in output frames
    float m_velocity{1.0f};         // Read rate (pitch)
    float m_position_spread{0.5f};  // Pan position [0, 1]
    bool m_active{false};
    size_t m_sample_index{0};  // Bank the grain reads from
    // Left / right (even / odd channel) gains for the current block, from
    // m_position_spread and Spread; refreshed by the engine every block.
    float m_gain_left{0.5f};
    float m_gain_right{0.5f};

    float phase() const {
        return static_cast<float>(m_current_position) / static_cast<float>(m_grain_size);
    }
    float source_position() const {
        return static_cast<float>(m_start_position) +
               static_cast<float>(m_current_position) * m_velocity;
    }
};

}  // namespace thl::dsp::granular
