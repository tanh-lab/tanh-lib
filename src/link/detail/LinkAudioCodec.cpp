#include "LinkAudioCodec.h"

#include <tanh/dsp/transport/LinkAudio.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace thl::link::detail {

using dsp::transport::LinkAudioPacket;

int16_t to_link_audio_sample(float sample) {
    return static_cast<int16_t>(std::lround(std::clamp(sample, -1.0f, 1.0f) * 32767.0f));
}

bool make_link_audio_packet(const int16_t* samples,
                            size_t num_channels,
                            size_t num_frames,
                            double begin_beat,
                            double end_beat,
                            LinkAudioPacket& out) {
    if (samples == nullptr || num_channels == 0 || num_frames == 0 || end_beat <= begin_beat) {
        return false;
    }
    out.m_num_channels = static_cast<uint32_t>(std::min<size_t>(num_channels, 2));
    const size_t frames = std::min(num_frames, LinkAudioPacket::k_max_samples / out.m_num_channels);
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
