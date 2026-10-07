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
    // Take what arrived; a full ring loses its oldest packet (and the playback position).
    while (true) {
        if (m_count == k_capacity) {
            pop_front();
            m_playing = false;
        }
        auto& slot = m_ring[(m_head + m_count) % k_capacity];
        if (!backend.pop(slot)) { break; }
        const bool usable = slot.m_num_frames > 0 && slot.m_num_channels > 0 &&
                            static_cast<size_t>(slot.m_num_frames) * slot.m_num_channels <=
                                LinkAudioPacket::k_max_samples &&
                            slot.m_end_beat > slot.m_begin_beat;
        if (usable) { ++m_count; }
    }

    if (num_frames == 0) { return m_playing; }
    if (beats_per_sample <= 0.0) {
        stop(out, num_channels, num_frames);
        return false;
    }
    const double end_beat = begin_beat + (static_cast<double>(num_frames) * beats_per_sample);

    // Start at the packet that holds the range's first beat, dropping everything before it
    // (after a timeline jump back, the packets before it belong to beats not reached yet).
    if (!m_playing) {
        size_t start = m_count;
        for (size_t i = 0; i < m_count; ++i) {
            const auto& p = packet(i);
            if (p.m_begin_beat <= begin_beat && begin_beat < p.m_end_beat) {
                start = i;
                break;
            }
        }
        if (start == m_count) {  // not arrived yet: drop what is older, keep what is newer
            while (m_count > 0 && packet(0).m_end_beat <= begin_beat) { pop_front(); }
            stop(out, num_channels, num_frames);
            return false;
        }
        for (size_t i = 0; i < start; ++i) { pop_front(); }
        m_read_frame = frame_at(packet(0), begin_beat);
    }

    // The source frames that cover the range: up to the packet holding its last beat.
    double frames_to_end = 0.0;
    bool covered = false;
    for (size_t i = 0; i < m_count; ++i) {
        const auto& p = packet(i);
        if (end_beat >= p.m_begin_beat && end_beat < p.m_end_beat) {
            frames_to_end += frame_at(p, end_beat);
            covered = true;
            break;
        }
        frames_to_end += static_cast<double>(p.m_num_frames);
    }
    const double source_frames = frames_to_end - m_read_frame;
    if (!covered || source_frames <= 0.0) {  // not arrived yet, or the timeline jumped
        if (!covered && m_playing) { ++m_underruns; }
        stop(out, num_channels, num_frames);
        return false;
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
    while (m_count > 0 && m_read_frame >= static_cast<double>(packet(0).m_num_frames)) {
        m_read_frame -= static_cast<double>(packet(0).m_num_frames);
        pop_front();
    }
    m_playing = true;
    m_margin_beats = m_count > 0 ? packet(m_count - 1).m_end_beat - end_beat : 0.0;
    return true;
}

}  // namespace thl::dsp::transport
