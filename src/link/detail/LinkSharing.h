#pragma once

// What the two Link Audio backends (C++ SDK, LinkKit) share: the received-buffer
// queue and the checks and conversion of a send. Private to tanh::Link.

#include <tanh/core/threading/LockFreeQueue.h>
#include <tanh/dsp/transport/LinkAudio.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "LinkAudioCodec.h"

namespace thl::link::detail {

/// Received buffers waiting for the audio thread (about 1.5 s of stereo at 48 kHz).
using ReceivedQueue = core::LockFreeQueue<dsp::transport::LinkAudioPacket, 256>;

/**
 * @brief What the session shares between the SDK's threads and the audio thread.
 *
 * The receiving callback may still run after its source is gone, so the session
 * keeps this alive (shared ownership) until every callback is done.
 */
struct SessionShared {
    ReceivedQueue m_received;
    /// The session's quantum: the clock quantizes with it, received audio is mapped with it.
    std::atomic<double> m_quantum{4.0};
};

/// Link thread: queues a received buffer mapped to local beats [@p begin, @p end). A full
/// queue drops it (the newest).
inline void push_received(ReceivedQueue& queue,
                          const int16_t* samples,
                          size_t num_channels,
                          size_t num_frames,
                          double begin,
                          double end) {
    dsp::transport::LinkAudioPacket packet;
    if (!make_link_audio_packet(samples, num_channels, num_frames, begin, end, packet)) { return; }
    (void)queue.try_push(packet);
}

/// The shape of one send: Link Audio carries mono or stereo.
struct SendShape {
    uint32_t m_channels = 0;
    uint32_t m_frames = 0;
    size_t m_samples = 0;  ///< interleaved samples
};

/// The shape of a send of @p num_channels x @p num_frames, or nullopt if there is nothing to send.
[[nodiscard]] inline std::optional<SendShape> send_shape(uint32_t num_channels,
                                                         uint32_t num_frames,
                                                         uint32_t sample_rate) {
    const uint32_t channels = std::min<uint32_t>(num_channels, 2);
    if (channels == 0 || num_frames == 0 || sample_rate == 0) { return std::nullopt; }
    return SendShape{.m_channels = channels,
                     .m_frames = num_frames,
                     .m_samples = static_cast<size_t>(num_frames) * channels};
}

/// Interleaves planar @p channels into Link Audio samples.
inline void interleave(const float* const* channels, const SendShape& shape, int16_t* out) {
    for (uint32_t f = 0; f < shape.m_frames; ++f) {
        for (uint32_t ch = 0; ch < shape.m_channels; ++ch) {
            out[(static_cast<size_t>(f) * shape.m_channels) + ch] =
                to_link_audio_sample(channels[ch][f]);
        }
    }
}

/**
 * @brief Hands the session state the clock captured to one send() of the same block.
 *
 * capture() marks a new capture; take() is true once per capture. A send with no
 * capture since the last one (no clock runs on the session) captures afresh
 * instead of committing a stale state.
 */
class CaptureTracker {
public:
    void captured() { ++m_captures; }
    [[nodiscard]] bool take() {
        if (m_taken == m_captures) { return false; }
        m_taken = m_captures;
        return true;
    }

private:
    uint64_t m_captures = 0;
    uint64_t m_taken = 0;
};

}  // namespace thl::link::detail
