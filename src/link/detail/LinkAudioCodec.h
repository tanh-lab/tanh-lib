#pragma once

// Link Audio's wire format (interleaved 16-bit samples) to and from the
// LinkAudioPacket seam type. Private to tanh::Link: both SDK backends use it.

#include <tanh/core/Exports.h>
#include <tanh/dsp/transport/LinkAudio.h>

#include <cstddef>
#include <cstdint>

namespace thl::link::detail {

/// A Link Audio sample from a float sample (clipped to -1..1).
[[nodiscard]] TANH_API int16_t to_link_audio_sample(float sample);

/**
 * @brief Fills @p out from a received Link Audio buffer (interleaved 16-bit samples) whose
 *        first frame is at local beat @p begin_beat and which ends at @p end_beat.
 *
 * Keeps at most two channels and LinkAudioPacket::k_max_samples samples; the end beat follows
 * the frames kept. Returns false for an empty buffer.
 */
TANH_API bool make_link_audio_packet(const int16_t* samples,
                                     size_t num_channels,
                                     size_t num_frames,
                                     double begin_beat,
                                     double end_beat,
                                     dsp::transport::LinkAudioPacket& out);

}  // namespace thl::link::detail
