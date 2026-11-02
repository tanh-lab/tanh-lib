#include <tanh/core/AbiNamespace.h>
#include <tanh/core/BufferView.h>
#include <tanh/dsp/dynamics/LevelDetector.h>
#include <tanh/dsp/probe/LevelProbe.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <vector>

namespace thl::inline THL_ABI_NAMESPACE::dsp::probe {

LevelProbe::LevelProbe(float rms_window_ms) : m_rms_window_ms(rms_window_ms) {}

void LevelProbe::prepare(double sample_rate, size_t /*max_block_size*/, size_t num_channels) {
    m_channels = std::vector<Channel>(num_channels);
    for (Channel& channel : m_channels) {
        channel.m_detector.prepare(sample_rate);
        channel.m_detector.set_mode(dynamics::DetectorMode::Rms);
        channel.m_detector.set_rms_window_ms(m_rms_window_ms);
    }
}

void LevelProbe::reset() TANH_NONBLOCKING_FUNCTION {
    for (Channel& channel : m_channels) {
        channel.m_detector.reset();
        channel.m_peak.store(0.0f, std::memory_order_relaxed);
        channel.m_mean_square.store(0.0f, std::memory_order_relaxed);
    }
}

void LevelProbe::observe(const thl::core::BufferView& block) TANH_NONBLOCKING_FUNCTION {
    const size_t channels = std::min(block.get_num_channels(), m_channels.size());
    for (size_t ch = 0; ch < channels; ++ch) {
        Channel& channel = m_channels[ch];
        const float* samples = block.get_read_pointer(ch);
        float block_peak = 0.0f;
        float mean_square = 0.0f;
        for (size_t i = 0; i < block.get_num_samples(); ++i) {
            block_peak = std::max(block_peak, std::abs(samples[i]));
            mean_square = channel.m_detector.process(samples[i]);
        }
        // The highest since the reader last took it: raise, never lower (the reader resets).
        float peak = channel.m_peak.load(std::memory_order_relaxed);
        while (block_peak > peak &&
               !channel.m_peak.compare_exchange_weak(peak, block_peak, std::memory_order_relaxed)) {
        }
        if (block.get_num_samples() > 0) {
            channel.m_mean_square.store(mean_square, std::memory_order_relaxed);
        }
    }
}

float LevelProbe::peak(size_t channel) const {
    return channel < m_channels.size() ? m_channels[channel].m_peak.load(std::memory_order_relaxed)
                                       : 0.0f;
}

float LevelProbe::take_peak(size_t channel) {
    return channel < m_channels.size()
               ? m_channels[channel].m_peak.exchange(0.0f, std::memory_order_relaxed)
               : 0.0f;
}

float LevelProbe::rms(size_t channel) const {
    return channel < m_channels.size()
               ? std::sqrt(m_channels[channel].m_mean_square.load(std::memory_order_relaxed))
               : 0.0f;
}

}  // namespace thl::inline THL_ABI_NAMESPACE::dsp::probe
