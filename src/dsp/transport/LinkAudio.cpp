#include "tanh/dsp/transport/LinkAudio.h"

#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace thl::dsp::transport {

namespace {

// Catmull-Rom through p[1]..p[2] at t in [0, 1).
float cubic(const std::array<float, 4>& p, float t) {
    const float a = (-0.5f * p[0]) + (1.5f * p[1]) - (1.5f * p[2]) + (0.5f * p[3]);
    const float b = p[0] - (2.5f * p[1]) + (2.0f * p[2]) - (0.5f * p[3]);
    const float c = (-0.5f * p[0]) + (0.5f * p[2]);
    return (((a * t + b) * t + c) * t) + p[1];
}

// Where @p beat falls in @p packet, in frames.
double frame_at(const LinkAudioPacket& packet, double beat) {
    return (beat - packet.m_begin_beat) / (packet.m_end_beat - packet.m_begin_beat) *
           static_cast<double>(packet.m_num_frames);
}

}  // namespace

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

LinkAudioReceiver::LinkAudioReceiver() : m_ring(k_capacity) {}

void LinkAudioReceiver::reset() TANH_NONBLOCKING_FUNCTION {
    m_head = 0;
    m_count = 0;
    m_playing = false;
    m_read_frame = 0.0;
}

void LinkAudioReceiver::pop_front() TANH_NONBLOCKING_FUNCTION {
    m_head = (m_head + 1) % k_capacity;
    --m_count;
}

float LinkAudioReceiver::sample(int64_t frame, uint32_t channel) const TANH_NONBLOCKING_FUNCTION {
    frame = std::max<int64_t>(frame, 0);  // before the first frame: hold it
    for (size_t i = 0; i < m_count; ++i) {
        const auto& p = packet(i);
        if (std::cmp_less(frame, p.m_num_frames)) {
            const uint32_t c = std::min(channel, p.m_num_channels - 1);
            return p.m_samples[(static_cast<size_t>(frame) * p.m_num_channels) + c];
        }
        frame -= static_cast<int64_t>(p.m_num_frames);
    }
    return 0.0f;
}

void LinkAudioReceiver::stop(float* const* out,
                             uint32_t num_channels,
                             uint32_t num_frames) TANH_NONBLOCKING_FUNCTION {
    m_playing = false;
    m_margin_beats = 0.0;
    m_read_frame = 0.0;
    for (uint32_t ch = 0; ch < num_channels; ++ch) { std::fill_n(out[ch], num_frames, 0.0f); }
}

bool LinkAudioReceiver::render(LinkAudioBackend& backend,
                               float* const* out,
                               uint32_t num_channels,
                               uint32_t num_frames,
                               double begin_beat,
                               double beats_per_sample) TANH_NONBLOCKING_FUNCTION {
    // Take what arrived. Packets come in beat order: one behind the last (reordered or repeated
    // by the network) is dropped, one far behind means the timeline jumped back (start over). A
    // full ring makes room by losing its oldest packet, and with it the playback position.
    while (backend.pop(m_incoming)) {
        ++m_received;
        const bool usable =
            m_incoming.m_num_frames > 0 && m_incoming.m_num_channels > 0 &&
            static_cast<size_t>(m_incoming.m_num_frames) * m_incoming.m_num_channels <=
                LinkAudioPacket::k_max_samples &&
            m_incoming.m_end_beat > m_incoming.m_begin_beat;
        if (!usable) { continue; }
        if (m_count > 0) {
            const double last_end = packet(m_count - 1).m_end_beat;
            if (m_incoming.m_begin_beat < last_end - k_jump_beats) {
                reset();
            } else if (m_incoming.m_begin_beat < last_end - k_order_tolerance_beats) {
                continue;
            }
        }
        if (m_count == k_capacity) {
            pop_front();
            m_playing = false;
        }
        m_ring[(m_head + m_count) % k_capacity] = m_incoming;
        ++m_count;
    }

    if (num_frames == 0) { return m_playing; }
    if (beats_per_sample <= 0.0) {
        stop(out, num_channels, num_frames);
        return false;
    }
    const double block_beats = static_cast<double>(num_frames) * beats_per_sample;
    const double end_beat = begin_beat + block_beats;

    // The range must continue where the last block ended. If it moved ahead of the read position
    // (the latency got shorter, the tempo changed, the clock seeked), start over at the new beat
    // instead of squeezing the audio in between into this block. A read position ahead of the
    // range is a skipped hole (the hole case below) or a jump back (stops below).
    if (m_playing && m_count > 0) {
        const auto& p = packet(0);
        const double read_beat =
            p.m_begin_beat +
            (m_read_frame / static_cast<double>(p.m_num_frames) * (p.m_end_beat - p.m_begin_beat));
        if (begin_beat - read_beat > k_seek_tolerance * block_beats) {
            m_playing = false;
        }
    }

    // Start at the first packet that reaches past the range's first beat (older ones go). A
    // packet that only starts later in the range is played from its start: the hole before it
    // was a lost packet. One that starts after the range has not arrived for it yet.
    if (!m_playing) {
        while (m_count > 0 && packet(0).m_end_beat <= begin_beat) { pop_front(); }
        if (m_count == 0 || packet(0).m_begin_beat > end_beat) {
            stop(out, num_channels, num_frames);
            return false;
        }
        m_read_frame = std::max(0.0, frame_at(packet(0), begin_beat));
    }

    // The source frames up to the range's last beat. A beat in a hole (a packet the network
    // lost) maps to the start of the next packet: the hole is skipped, not a dropout. Only a
    // beat past the last packet is missing audio.
    double frames_to_end = 0.0;
    bool covered = false;
    for (size_t i = 0; i < m_count; ++i) {
        const auto& p = packet(i);
        if (end_beat < p.m_end_beat) {
            frames_to_end += frame_at(p, std::max(end_beat, p.m_begin_beat));
            covered = true;
            break;
        }
        frames_to_end += static_cast<double>(p.m_num_frames);
    }
    const double source_frames = frames_to_end - m_read_frame;
    if (!covered || source_frames < -k_frame_epsilon) {  // not arrived yet, or jumped back
        if (!covered && m_playing) { ++m_underruns; }
        stop(out, num_channels, num_frames);
        return false;
    }
    if (source_frames <= k_frame_epsilon) {  // the whole range lies in a hole: its audio was lost
        for (uint32_t ch = 0; ch < num_channels; ++ch) { std::fill_n(out[ch], num_frames, 0.0f); }
        m_playing = true;
        return true;
    }

    // Read at the rate that maps the range onto the block (tempo and sample rate follow).
    const double step = source_frames / static_cast<double>(num_frames);
    for (uint32_t f = 0; f < num_frames; ++f) {
        const double position = m_read_frame + (static_cast<double>(f) * step);
        const double whole = std::floor(position);
        const auto index = static_cast<int64_t>(whole);
        const auto t = static_cast<float>(position - whole);
        for (uint32_t ch = 0; ch < num_channels; ++ch) {
            const std::array<float, 4> points{sample(index - 1, ch),
                                              sample(index, ch),
                                              sample(index + 1, ch),
                                              sample(index + 2, ch)};
            out[ch][f] = cubic(points, t);
        }
    }
    m_read_frame += source_frames;
    while (m_count > 0 &&
           m_read_frame >= static_cast<double>(packet(0).m_num_frames) - k_frame_epsilon) {
        m_read_frame = std::max(0.0, m_read_frame - static_cast<double>(packet(0).m_num_frames));
        pop_front();
    }
    m_playing = true;
    m_margin_beats = m_count > 0 ? packet(m_count - 1).m_end_beat - end_beat : 0.0;
    return true;
}

}  // namespace thl::dsp::transport
