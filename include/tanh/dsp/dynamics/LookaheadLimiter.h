#pragma once

#include <tanh/core/AbiNamespace.h>
#include <tanh/core/Buffer.h>
#include <tanh/core/BufferView.h>
#include <tanh/core/Exports.h>
#include <tanh/dsp/BaseProcessor.h>
#include <tanh/dsp/dynamics/LevelDetector.h>
#include <tanh/dsp/utils/DelayProcessor.h>
#include <tanh/dsp/utils/SmoothedValue.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace thl::inline THL_ABI_NAMESPACE::dsp::dynamics {

/**
 * @brief A brickwall lookahead limiter: no output sample above the ceiling, without clipping.
 *
 * For every key sample the gain that keeps it at the ceiling, min(1, ceiling / peak), goes
 * through a minimum over the lookahead (L samples: the gain is down before a peak arrives and
 * held while it passes), a release in dB, and a moving average over L (the gain ramps down
 * linearly instead of jumping). The audio is delayed by L - 1, which puts every peak where the
 * average has fully reached its gain: the output stays at the ceiling exactly, a hard clip at
 * the ceiling only catches rounding (and the moments after a lookahead change).
 *
 * - True peak: the key is interpolated 4x (a 97-tap Kaiser-windowed sinc), so the peaks between
 *   the samples count too (ITU-R BS.1770's true peak). Measured 16x, the output stays at the
 *   ceiling for content up to 0.44 fs (21 kHz at 48 kHz); only content right at Nyquist (white
 *   noise) overshoots, by about 1 dB. Adds k_true_peak_delay samples of latency.
 * - Link: Linked (the default; StereoLink::Average counts as linked) gives all channels the
 *   loudest's gain, DualMono each its own.
 * - Input gain drives into the limiter (and glides over k_ramp_seconds).
 *
 * latency_samples() is L - 1 (+ k_true_peak_delay); max_latency_samples() its maximum.
 *
 * A derived class gives every Parameter through get_parameter_float(), read at the start of
 * every process() call at @p modulation_offset; non-finite values become the defaults, the rest
 * are clamped. A lookahead change restarts the gain's averaging (the audio delay crossfades).
 * Up to k_max_channels channels; further ones pass delayed
 * and driven by the input gain, unlimited.
 *
 * Threads: prepare() allocates (audio stopped); process() and reset() are real-time safe.
 */
class TANH_API LookaheadLimiterImpl : public BaseProcessor {
public:
    static constexpr size_t k_max_channels = 16;
    static constexpr float k_max_lookahead_ms = 10.0f;
    static constexpr double k_ramp_seconds = 0.02;
    static constexpr size_t k_true_peak_factor = 4;
    static constexpr size_t k_true_peak_taps_per_phase = 25;  // 97 taps: 4 · 24 + 1
    /// The interpolator's group delay.
    static constexpr uint32_t k_true_peak_delay =
        static_cast<uint32_t>((k_true_peak_taps_per_phase - 1) / 2);

    void prepare(const double& sample_rate,
                 const size_t& samples_per_block,
                 const size_t& num_channels) override;
    void process(thl::core::BufferView buffer,
                 uint32_t modulation_offset = 0) TANH_NONBLOCKING_FUNCTION override;
    /// Clears the lookahead, the interpolator and the gain (none held).
    void reset() TANH_NONBLOCKING_FUNCTION override;

    /// The last block's deepest gain reduction in dB (<= 0).
    [[nodiscard]] float gain_reduction_db() const {
        return m_meter_gain_db.load(std::memory_order_relaxed);
    }
    /// The last block's output sample peak in dBFS.
    [[nodiscard]] float output_peak_db() const {
        return m_meter_output_db.load(std::memory_order_relaxed);
    }

protected:
    enum Parameter {
        InputGain = 0,  ///< dB, [-24, 24], default 0
        Ceiling,        ///< dBFS, [-30, 0], default -1
        Lookahead,      ///< ms, [1 sample, k_max_lookahead_ms], default 2
        Release,        ///< ms, [0, 2000], default 100
        TruePeak,       ///< > 0.5 on, default off
        Link,           ///< StereoLink, default Linked

        NumParameters
    };

    virtual float get_parameter_float(Parameter parameter, uint32_t modulation_offset = 0) = 0;

private:
    /// One gain path: minimum over the lookahead, release, moving average.
    struct GainStage {
        std::vector<float> m_min_values;  // monotonic queue, a ring
        std::vector<uint64_t> m_min_times;
        size_t m_min_head = 0;
        size_t m_min_size = 0;
        std::vector<float> m_average;  // the last L released gains, a ring
        size_t m_average_position = 0;
        double m_sum = 0.0;
        float m_released_db = 0.0f;
    };
    /// One channel's true-peak interpolator: the last taps-per-phase samples.
    struct Interpolator {
        std::array<float, k_true_peak_taps_per_phase> m_history{};
        size_t m_position = 0;
    };

    [[nodiscard]] float parameter(Parameter parameter,
                                  uint32_t modulation_offset,
                                  float fallback,
                                  float lowest,
                                  float highest) TANH_NONBLOCKING_FUNCTION;
    void read_parameters(uint32_t modulation_offset) TANH_NONBLOCKING_FUNCTION;
    void restart(GainStage& stage, float gain) TANH_NONBLOCKING_FUNCTION;
    [[nodiscard]] float advance(GainStage& stage, float required) TANH_NONBLOCKING_FUNCTION;
    [[nodiscard]] float key_peak(Interpolator& interpolator,
                                 float x) const TANH_NONBLOCKING_FUNCTION;

    double m_sample_rate = 48000.0;
    size_t m_num_channels = 0;
    size_t m_max_lookahead = 1;  // samples
    size_t m_lookahead = 1;      // L, samples
    bool m_true_peak = false;
    bool m_linked = true;
    float m_ceiling = 1.0f;
    float m_input_gain_target = 1.0f;
    float m_release_coefficient = 0.0f;
    float m_release_ms = -1.0f;
    uint64_t m_time = 0;
    std::array<std::array<float, k_true_peak_taps_per_phase>, k_true_peak_factor> m_phases{};
    std::vector<GainStage> m_stages;            // per channel (linked: the first)
    std::vector<Interpolator> m_interpolators;  // per channel
    std::vector<float> m_peaks;                 // per channel, one sample
    thl::core::Buffer<float> m_gains;           // per channel, the block's gain
    thl::dsp::utils::DelayProcessor m_delay;
    thl::dsp::utils::SmoothedValue m_input_gain;
    std::atomic<float> m_meter_gain_db{0.0f};
    std::atomic<float> m_meter_output_db{-120.0f};
};

}  // namespace thl::inline THL_ABI_NAMESPACE::dsp::dynamics
