#pragma once

#include <tanh/core/Buffer.h>
#include <tanh/core/BufferView.h>
#include <tanh/core/Exports.h>
#include <tanh/dsp/BaseProcessor.h>
#include <tanh/dsp/dynamics/GainComputer.h>
#include <tanh/dsp/dynamics/GainSmoother.h>
#include <tanh/dsp/dynamics/LevelDetector.h>
#include <tanh/dsp/filter/SvfCoefficients.h>
#include <tanh/dsp/utils/DelayProcessor.h>
#include <tanh/dsp/utils/SmoothedValue.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace thl::dsp::dynamics {

/**
 * @brief A compressor and expander: downward and upward, with lookahead, sidechain and mix.
 *
 * The key signal (the input, or the sidechain of process_with_sidechain()) passes an optional
 * 12 dB/octave high-pass, its level is detected (LevelDetector: peak or RMS) and linked across
 * the channels (StereoLink), the gain computer (GainComputer) turns it into a gain in dB and the
 * smoother (GainSmoother: attack, hold, release, auto release) gives that gain its ballistics.
 * The audio is delayed by the lookahead, so the gain moves before what it reacts to;
 * latency_samples() is the lookahead (its maximum, k_max_lookahead_ms, is max_latency_samples()).
 *
 * The output is the delayed input times (1 - mix) + mix · gain · makeup: a mix below 1 is
 * parallel compression, with the dry part inside, delayed as the wet one. The compressor shifts
 * no phase, so process_with_dry() is BaseProcessor's (the input, undelayed, as its contract says).
 * Auto makeup adds half the gain the curve gives at 0 dBFS, negated, to the makeup.
 *
 * A derived class gives every Parameter through get_parameter_float(); all are read at the start
 * of every process() call, at @p modulation_offset (process_modulated() applies them at its
 * change points). Non-finite values become the defaults below; the rest are clamped. Enums come
 * as floats, rounded. Makeup and mix glide over k_ramp_seconds. Up to k_max_channels channels;
 * further ones pass unchanged (delayed by the lookahead).
 *
 * The meters (any thread) hold the last block's largest gain change, input and output peak.
 *
 * Threads: prepare() allocates (audio stopped); process(), process_with_sidechain() and reset()
 * are real-time safe.
 */
class TANH_API CompressorImpl : public BaseProcessor {
public:
    static constexpr size_t k_max_channels = 16;
    static constexpr float k_max_lookahead_ms = 20.0f;
    static constexpr double k_ramp_seconds = 0.02;
    static constexpr float k_max_auto_makeup_db = 24.0f;

    void prepare(const double& sample_rate,
                 const size_t& samples_per_block,
                 const size_t& num_channels) override;
    void process(thl::core::BufferView buffer,
                 uint32_t modulation_offset = 0) TANH_NONBLOCKING_FUNCTION override;
    /// process() keyed by @p sidechain instead of the input: its channel c drives channel c (its
    /// last channel the ones beyond). An empty @p sidechain keys by the input. Same length as
    /// @p buffer.
    void process_with_sidechain(const thl::core::BufferView& buffer,
                                const thl::core::BufferView& sidechain,
                                uint32_t modulation_offset = 0) TANH_NONBLOCKING_FUNCTION;
    /// Clears the detectors, the gain, the sidechain filter and the lookahead delay; the glides
    /// end at their targets.
    void reset() TANH_NONBLOCKING_FUNCTION override;

    /// The last block's gain change farthest from 0 dB (negative: reduction), in dB.
    [[nodiscard]] float gain_change_db() const {
        return m_meter_gain_db.load(std::memory_order_relaxed);
    }
    /// The last block's input peak in dBFS (the key signal's, before the high-pass).
    [[nodiscard]] float input_peak_db() const {
        return m_meter_input_db.load(std::memory_order_relaxed);
    }
    /// The last block's output peak in dBFS.
    [[nodiscard]] float output_peak_db() const {
        return m_meter_output_db.load(std::memory_order_relaxed);
    }

protected:
    enum Parameter {
        Threshold = 0,  ///< dB, [-80, 0], default -18
        Ratio,          ///< [1, inf], default 4; infinity limits (downward) or gates (expansion)
        Knee,           ///< dB, [0, 24], default 6
        Range,          ///< dB, the largest gain change, [0, 60], default 60
        Mode,           ///< DynamicsMode, default DownwardCompression
        Attack,         ///< ms, [0, 500], default 10
        Hold,           ///< ms, [0, 500], default 0
        Release,        ///< ms, [0, 5000], default 100
        AutoRelease,    ///< > 0.5 on, default off
        Lookahead,      ///< ms, [0, k_max_lookahead_ms], default 0
        Detector,       ///< DetectorMode, default Peak
        RmsWindow,      ///< ms, [0.1, 1000], default 10
        Link,           ///< StereoLink, default Linked
        SidechainHighPass,  ///< Hz, below 10 off (the default), else [10, 0.49 fs]
        Makeup,             ///< dB, [-24, 24], default 0
        AutoMakeup,         ///< > 0.5 on, default off
        Mix,                ///< [0, 1], default 1

        NumParameters
    };

    virtual float get_parameter_float(Parameter parameter, uint32_t modulation_offset = 0) = 0;

private:
    struct Settings {
        GainComputer m_curve;
        float m_attack_ms = 10.0f;
        float m_hold_ms = 0.0f;
        float m_release_ms = 100.0f;
        bool m_auto_release = false;
        float m_lookahead_ms = 0.0f;
        DetectorMode m_detector = DetectorMode::Peak;
        float m_rms_window_ms = 10.0f;
        StereoLink m_link = StereoLink::Linked;
        float m_high_pass_hz = 0.0f;
        float m_makeup_db = 0.0f;
        bool m_auto_makeup = false;
        float m_mix = 1.0f;
    };

    [[nodiscard]] float parameter(Parameter parameter,
                                  uint32_t modulation_offset,
                                  float fallback,
                                  float lowest,
                                  float highest) TANH_NONBLOCKING_FUNCTION;
    void read_parameters(uint32_t modulation_offset) TANH_NONBLOCKING_FUNCTION;
    void apply_settings() TANH_NONBLOCKING_FUNCTION;
    [[nodiscard]] float total_makeup_db() const TANH_NONBLOCKING_FUNCTION;
    void run(thl::core::BufferView buffer,
             const thl::core::BufferView& key,
             uint32_t modulation_offset) TANH_NONBLOCKING_FUNCTION;

    double m_sample_rate = 48000.0;
    size_t m_num_channels = 0;  // prepared, at most k_max_channels
    Settings m_settings;
    float m_applied_high_pass_hz = -1.0f;  // the filter designed last
    bool m_high_pass_on = false;
    thl::dsp::filter::SvfCoefficients m_high_pass;
    std::vector<thl::dsp::filter::SvfState> m_high_pass_state;  // per channel
    std::vector<LevelDetector> m_detectors;                     // per channel
    std::vector<GainSmoother> m_smoothers;                      // per channel (linked: the first)
    std::vector<float> m_powers;                                // per channel, one sample
    thl::core::Buffer<float> m_gains;                           // per channel, the block's dB
    thl::dsp::utils::DelayProcessor m_lookahead;
    thl::dsp::utils::SmoothedValue m_makeup;  // dB
    thl::dsp::utils::SmoothedValue m_mix;
    std::atomic<float> m_meter_gain_db{0.0f};
    std::atomic<float> m_meter_input_db{k_floor_db};
    std::atomic<float> m_meter_output_db{k_floor_db};
};

}  // namespace thl::dsp::dynamics
