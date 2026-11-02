#pragma once

#include <tanh/core/AbiNamespace.h>
#include <tanh/core/BufferView.h>
#include <tanh/core/Exports.h>
#include <tanh/dsp/dynamics/LevelDetector.h>
#include <tanh/dsp/probe/Probe.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <atomic>
#include <cstddef>
#include <vector>

namespace thl::inline THL_ABI_NAMESPACE::dsp::probe {

/**
 * @brief Peak and RMS of every channel, measured on the audio thread, read from any thread.
 *
 * - Peak: the highest |x| since the reader last took it (take_peak()), so a reader polling at
 *   screen rate misses no peak between two of its frames; peak() looks without taking.
 * - RMS: a one-pole mean square over rms_window_ms() (300 ms by default, a VU's integration
 *   time), the value at the end of the last block.
 * Both linear (dsp::utils::gain_to_db() for dB). Readers on several threads may read; take_peak()
 * is for one of them.
 */
class TANH_API LevelProbe : public ProbeBase {
public:
    static constexpr float k_default_rms_window_ms = 300.0f;

    explicit LevelProbe(float rms_window_ms = k_default_rms_window_ms);

    void prepare(double sample_rate, size_t max_block_size, size_t num_channels) override;
    void reset() TANH_NONBLOCKING_FUNCTION override;

    [[nodiscard]] size_t num_channels() const { return m_channels.size(); }
    [[nodiscard]] float rms_window_ms() const { return m_rms_window_ms; }

    /// The highest |x| of channel @p channel since the last take_peak() (0 for no channel).
    [[nodiscard]] float peak(size_t channel) const;
    /// peak(), and starts the next one from 0.
    float take_peak(size_t channel);
    /// The RMS of channel @p channel at the end of the last block (0 for no channel).
    [[nodiscard]] float rms(size_t channel) const;

protected:
    void observe(const thl::core::BufferView& block) TANH_NONBLOCKING_FUNCTION override;

private:
    struct Channel {
        dynamics::LevelDetector m_detector;  // audio thread only
        std::atomic<float> m_peak{0.0f};
        std::atomic<float> m_mean_square{0.0f};
    };

    float m_rms_window_ms;
    std::vector<Channel> m_channels;
};

}  // namespace thl::inline THL_ABI_NAMESPACE::dsp::probe
