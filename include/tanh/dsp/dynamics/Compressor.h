#pragma once

#include <tanh/core/AbiNamespace.h>
#include <tanh/core/Buffer.h>
#include <tanh/core/BufferView.h>
#include <tanh/core/Exports.h>
#include <tanh/dsp/BaseProcessor.h>
#include <tanh/dsp/dynamics/GainComputer.h>
#include <tanh/dsp/dynamics/GainSmoother.h>
#include <tanh/dsp/dynamics/LevelDetector.h>
#include <tanh/dsp/dynamics/StereoLink.h>
#include <tanh/dsp/filter/SvfCoefficients.h>
#include <tanh/dsp/utils/Decibels.h>
#include <tanh/dsp/utils/DelayProcessor.h>
#include <tanh/dsp/utils/ParameterValue.h>
#include <tanh/dsp/utils/SmoothedValue.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace thl::inline THL_ABI_NAMESPACE::dsp::dynamics {

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
 * change points). A NaN becomes the parameter's default (k_parameter_ranges), the rest is
 * clamped. Enums come as floats, rounded. Makeup and mix glide over k_ramp_seconds. Up to
 * k_max_channels channels; further ones pass unchanged (delayed by the lookahead).
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
        Threshold = 0,      ///< dB
        Ratio,              ///< up to infinity: limits (downward) or gates (expansion)
        Knee,               ///< dB
        Range,              ///< dB, the largest gain change
        Mode,               ///< DynamicsMode
        Attack,             ///< ms
        Hold,               ///< ms
        Release,            ///< ms
        AutoRelease,        ///< > 0.5 on
        Lookahead,          ///< ms
        Detector,           ///< DetectorMode
        RmsWindow,          ///< ms
        Link,               ///< StereoLink
        SidechainHighPass,  ///< Hz; below 10 off, capped at 0.49 fs
        Makeup,             ///< dB
        AutoMakeup,         ///< > 0.5 on
        Mix,                ///< 0 dry to 1 wet

        NumParameters
    };

    /// Every parameter's default and range (enums: their values as floats), e.g. for a host's
    /// parameter definitions. A NaN reads as the default, the rest is clamped.
    static constexpr std::array<utils::ParameterRange, NumParameters> k_parameter_ranges{{
        {.m_default = -18.0f, .m_min = -80.0f, .m_max = 0.0f},  // Threshold
        {.m_default = 4.0f,
         .m_min = 1.0f,
         .m_max = std::numeric_limits<float>::infinity()},                // Ratio
        {.m_default = 6.0f, .m_min = 0.0f, .m_max = 24.0f},               // Knee
        {.m_default = 60.0f, .m_min = 0.0f, .m_max = 60.0f},              // Range
        {.m_default = 0.0f, .m_min = 0.0f, .m_max = 3.0f},                // Mode
        {.m_default = 10.0f, .m_min = 0.0f, .m_max = 500.0f},             // Attack
        {.m_default = 0.0f, .m_min = 0.0f, .m_max = 500.0f},              // Hold
        {.m_default = 100.0f, .m_min = 0.0f, .m_max = 5000.0f},           // Release
        {.m_default = 0.0f, .m_min = 0.0f, .m_max = 1.0f},                // AutoRelease
        {.m_default = 0.0f, .m_min = 0.0f, .m_max = k_max_lookahead_ms},  // Lookahead
        {.m_default = 0.0f, .m_min = 0.0f, .m_max = 1.0f},                // Detector
        {.m_default = 10.0f, .m_min = 0.1f, .m_max = 1000.0f},            // RmsWindow
        {.m_default = 0.0f, .m_min = 0.0f, .m_max = 2.0f},                // Link
        {.m_default = 0.0f, .m_min = 0.0f, .m_max = 20000.0f},            // SidechainHighPass
        {.m_default = 0.0f, .m_min = -24.0f, .m_max = 24.0f},             // Makeup
        {.m_default = 0.0f, .m_min = 0.0f, .m_max = 1.0f},                // AutoMakeup
        {.m_default = 1.0f, .m_min = 0.0f, .m_max = 1.0f},                // Mix
    }};

    virtual float get_parameter_float(Parameter parameter, uint32_t modulation_offset = 0) = 0;

private:
    struct Settings {  // as read last (read_parameters(): k_parameter_ranges)
        GainComputer m_curve;
        float m_attack_ms{};
        float m_hold_ms{};
        float m_release_ms{};
        bool m_auto_release{};
        float m_lookahead_ms{};
        DetectorMode m_detector{};
        float m_rms_window_ms{};
        StereoLink m_link{};
        float m_high_pass_hz{};
        float m_makeup_db{};
        bool m_auto_makeup{};
        float m_mix{};
    };

    [[nodiscard]] float value(Parameter parameter,
                              uint32_t modulation_offset) TANH_NONBLOCKING_FUNCTION;
    [[nodiscard]] bool switch_on(Parameter parameter,
                                 uint32_t modulation_offset) TANH_NONBLOCKING_FUNCTION;
    template <typename Enum>
    [[nodiscard]] Enum choice(Parameter parameter,
                              uint32_t modulation_offset,
                              Enum last) TANH_NONBLOCKING_FUNCTION;
    void read_parameters(uint32_t modulation_offset) TANH_NONBLOCKING_FUNCTION;
    void apply_settings() TANH_NONBLOCKING_FUNCTION;
    [[nodiscard]] float total_makeup_db() const TANH_NONBLOCKING_FUNCTION;
    void run(thl::core::BufferView buffer,
             const thl::core::BufferView& key,
             uint32_t modulation_offset) TANH_NONBLOCKING_FUNCTION;

    double m_sample_rate = 48000.0;
    size_t m_num_channels = 0;  // prepared, at most k_max_channels
    Settings m_settings;
    bool m_linked = true;                  // as applied last: only the first smoother runs
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
    std::atomic<float> m_meter_input_db{utils::k_min_db};
    std::atomic<float> m_meter_output_db{utils::k_min_db};
};

}  // namespace thl::inline THL_ABI_NAMESPACE::dsp::dynamics
