#include "tanh/modulation/detail/XYFramePublisher.h"

#include <tanh/modulation/XYFrame.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace thl::modulation::detail {

namespace {
uint64_t hash_combine(uint64_t hash, uint64_t value) {
    hash ^= value + 0x9E3779B97F4A7C15ull + (hash << 6) + (hash >> 2);
    return hash;
}
}  // namespace

void XYFramePublisher::prepare(double sample_rate, double frame_rate_hz, uint32_t trail_interval) {
    const double hz = frame_rate_hz > 0.0 ? frame_rate_hz : 240.0;
    m_frame_interval = static_cast<uint32_t>(std::max(1.0, std::floor(sample_rate / hz)));
    m_trail_interval = std::max<uint32_t>(trail_interval, 1);
    m_since_publish = m_frame_interval;  // publish the first block
}

void XYFramePublisher::add_trail(uint8_t voice,
                                 const float* x,
                                 const float* y,
                                 const uint8_t* gate,
                                 uint32_t num_samples,
                                 float progress,
                                 uint32_t& countdown) TANH_NONBLOCKING_FUNCTION {
    // Drop-oldest when the UI does not drain.
    uint32_t i = std::min(countdown, num_samples);
    for (; i < num_samples; i += m_trail_interval) {
        const XYPathPoint p{.m_x = x[i],
                            .m_y = y[i],
                            .m_progress = progress,
                            .m_voice = voice,
                            .m_active = gate[i]};
        m_trail_dropped += static_cast<uint32_t>(m_trail.push_overwrite(p));
    }
    countdown = i - num_samples;
}

void XYFramePublisher::publish(uint64_t sample_time,
                               uint32_t num_voices,
                               bool latch,
                               uint32_t num_samples) TANH_NONBLOCKING_FUNCTION {
    XYFrame& frame = m_frames.write_buffer();
    frame.m_sample_time = sample_time;
    frame.m_num_voices = num_voices;
    frame.m_trail_dropped = m_trail_dropped;
    frame.m_latch = latch;
    for (uint32_t v = num_voices; v < XYFrame::k_max_voices; ++v) {
        frame.m_voices[v] = XYVoiceFrame{};
    }

    uint64_t signature = latch ? 1u : 0u;
    for (uint32_t v = 0; v < num_voices; ++v) {
        const auto& f = frame.m_voices[v];
        signature = hash_combine(signature, static_cast<uint64_t>(f.m_motion.m_state));
        signature = hash_combine(signature, f.m_motion.m_take_id);
        signature = hash_combine(signature, f.m_path_version);
        signature = hash_combine(signature, static_cast<uint64_t>(f.m_layer));
        signature = hash_combine(signature,
                                 (f.m_active != 0 ? 1u : 0u) | (f.m_touched ? 2u : 0u) |
                                     (f.m_motion.m_playing ? 4u : 0u) |
                                     (f.m_motion.m_reverse ? 8u : 0u) | (f.m_has_value ? 16u : 0u));
    }
    m_since_publish += num_samples;
    if (signature == m_last_signature && m_since_publish < m_frame_interval && m_sequence > 0) {
        return;
    }
    m_last_signature = signature;
    m_since_publish = 0;
    frame.m_sequence = ++m_sequence;
    m_frames.publish();
}

size_t XYFramePublisher::drain(std::span<XYPathPoint> out) {
    size_t n = 0;
    while (n < out.size() && m_trail.try_pop(out[n])) { ++n; }
    return n;
}

}  // namespace thl::modulation::detail
