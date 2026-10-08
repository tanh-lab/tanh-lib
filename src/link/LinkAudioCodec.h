#pragma once

// Link Audio's wire format (interleaved 16-bit samples) to and from the
// LinkAudioPacket seam type. Private to tanh::Link (header-only: nothing exported);
// both SDK backends use it.

#include <tanh/dsp/transport/LinkAudio.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace thl::link::detail {

/// A Link Audio sample from a float sample (clipped to -1..1).
[[nodiscard]] inline int16_t to_link_audio_sample(float sample) {
    return static_cast<int16_t>(std::lround(std::clamp(sample, -1.0f, 1.0f) * 32767.0f));
}

/**
 * @brief Fills @p out from a received Link Audio buffer (interleaved 16-bit samples) whose
 *        first frame is at local beat @p begin_beat and which ends at @p end_beat.
 *
 * Keeps at most two channels and LinkAudioPacket::k_max_samples samples; the end beat follows
 * the frames kept. Returns false for an empty buffer.
 */
inline bool make_link_audio_packet(const int16_t* samples,
                                   size_t num_channels,
                                   size_t num_frames,
                                   double begin_beat,
                                   double end_beat,
                                   dsp::transport::LinkAudioPacket& out) {
    if (samples == nullptr || num_channels == 0 || num_frames == 0 || end_beat <= begin_beat) {
        return false;
    }
    out.m_num_channels = static_cast<uint32_t>(std::min<size_t>(num_channels, 2));
    const size_t frames =
        std::min(num_frames, dsp::transport::LinkAudioPacket::k_max_samples / out.m_num_channels);
    out.m_num_frames = static_cast<uint32_t>(frames);
    for (size_t f = 0; f < frames; ++f) {
        for (size_t ch = 0; ch < out.m_num_channels; ++ch) {
            out.m_samples[(f * out.m_num_channels) + ch] =
                static_cast<float>(samples[(f * num_channels) + ch]) / 32768.0f;
        }
    }
    out.m_begin_beat = begin_beat;
    out.m_end_beat = begin_beat + ((end_beat - begin_beat) * static_cast<double>(frames) /
                                   static_cast<double>(num_frames));
    return true;
}

}  // namespace thl::link::detail
