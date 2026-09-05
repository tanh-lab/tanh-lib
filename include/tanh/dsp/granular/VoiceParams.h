#pragma once

#include <tanh/dsp/granular/GranularTypes.h>

namespace thl::dsp::granular {

// One block's worth of voice parameters, read from the host's parameter
// system exactly once per process() call and clamped here — the modulation
// system hands out unclamped plain-space values by contract, so this is the
// single place a component may rely on ranges. Components below the voice
// take a `const VoiceParams&` and never touch the parameter system.
struct VoiceParams {
    bool m_playing{false};
    float m_volume{1.0f};

    float m_size{0.5f};     // [0, 1]
    float m_density{0.5f};  // [0, 1]
    // Raw (unclamped): per-grain pitch factor in the granular modes, the
    // head's own rate (varispeed) in Sample mode. Each engine bounds it for
    // its own use — the bounds differ on purpose.
    float m_velocity{1.0f};

    float m_temperature_size{0.0f};      // [0, 1]
    float m_temperature_position{0.0f};  // [0, 1]
    float m_temperature_velocity{0.0f};  // [0, 1]

    // Pitch-bank index, raw: consumers clamp against the bank count.
    int m_sample_index{0};
    float m_sample_start{0.0f};       // [0, 1] of the sample
    float m_sample_end{1.0f};         // [0, 1]
    float m_sample_loop_point{0.0f};  // [0, 1]

    ChannelMode m_channel_mode{ChannelMode::MonoToStereo};
    float m_spread{0.0f};  // [0, 1]

    EngineMode m_engine_mode{EngineMode::GranularLoop};
    float m_position{0.0f};  // [0, 1]
    float m_spray{0.0f};     // [0, 1]
    float m_tilt{0.0f};      // [-1, 1]

    // Grain window: shape morph position [0, MorphWindow::k_max_shape]
    // (integers = exact shapes, default Hann) and tilt [-1, 1].
    float m_window_shape{4.0f};
    float m_window_tilt{0.0f};

    // Master ADSR, as delivered (the ADSR validates its own inputs).
    float m_env_attack{0.0f};
    float m_env_decay{0.0f};
    float m_env_sustain{1.0f};
    float m_env_release{0.0f};
    float m_env_attack_curve{0.0f};
    float m_env_decay_curve{0.0f};
    float m_env_release_curve{0.0f};
};

}  // namespace thl::dsp::granular
