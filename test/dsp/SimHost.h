#pragma once

// Deterministic simulated host playhead for transport tests.
//
// A script of sample-stamped events (seek, tempo, play, stop, loop) drives a
// host the way a DAW does: changes requested anywhere inside a block become
// visible at the next block boundary (hosts report one position per block), a
// cycle wraps at the first block start past the loop end, and the beat moves
// at the current tempo only while playing. next_block() returns the host's
// per-block TransportInfo (validity flags, no discontinuity bits) — what a
// JUCE/CLAP/VST3 adapter would hand to HostTransportClock::set_host_info().

#include <tanh/dsp/transport/TransportInfo.h>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace sim {

struct Event {
    enum class Kind { Seek, Tempo, Play, Stop, Loop, LoopOff };
    uint64_t m_sample = 0;
    Kind m_kind = Kind::Seek;
    double m_a = 0.0;
    double m_b = 0.0;
};

class SimHost {
public:
    explicit SimHost(double sample_rate, double bpm = 120.0)
        : m_sample_rate(sample_rate), m_bpm(bpm) {}

    void add(Event e) {
        m_script.push_back(e);
        std::stable_sort(m_script.begin(), m_script.end(), [](const Event& a, const Event& b) {
            return a.m_sample < b.m_sample;
        });
    }

    // Validity bits reported per block (default: a full-featured host).
    uint32_t m_report = thl::dsp::transport::TransportInfo::k_has_tempo |
                        thl::dsp::transport::TransportInfo::k_has_beat_position |
                        thl::dsp::transport::TransportInfo::k_has_time_signature |
                        thl::dsp::transport::TransportInfo::k_has_loop;

    thl::dsp::transport::TransportInfo next_block(uint32_t frames) {
        using thl::dsp::transport::TransportInfo;
        // Apply everything requested before this block starts.
        while (m_next < m_script.size() && m_script[m_next].m_sample <= m_now) {
            apply(m_script[m_next++]);
        }
        if (m_looping && m_playing && m_beat >= m_loop_end) {
            m_beat = m_loop_start + (m_beat - m_loop_end);
        }

        TransportInfo info;
        info.m_flags = m_report;
        if (m_playing) { info.m_flags |= TransportInfo::k_is_playing; }
        if (m_looping) { info.m_flags |= TransportInfo::k_is_looping; }
        info.m_bpm = m_bpm;
        info.m_beat_position = m_beat;
        info.m_loop_start_beats = m_loop_start;
        info.m_loop_end_beats = m_loop_end;
        info.m_sig_num = 4;
        info.m_sig_denom = 4;

        m_last_start = m_beat;
        if (m_playing) { m_beat += static_cast<double>(frames) * m_bpm / (60.0 * m_sample_rate); }
        m_now += frames;
        return info;
    }

    [[nodiscard]] uint64_t now() const { return m_now; }
    [[nodiscard]] double last_start() const { return m_last_start; }

private:
    void apply(const Event& e) {
        switch (e.m_kind) {
            case Event::Kind::Seek: m_beat = e.m_a; break;
            case Event::Kind::Tempo: m_bpm = e.m_a; break;
            case Event::Kind::Play: m_playing = true; break;
            case Event::Kind::Stop: m_playing = false; break;
            case Event::Kind::Loop:
                m_looping = true;
                m_loop_start = e.m_a;
                m_loop_end = e.m_b;
                break;
            case Event::Kind::LoopOff: m_looping = false; break;
        }
    }

    std::vector<Event> m_script;
    size_t m_next = 0;
    double m_sample_rate;
    double m_bpm;
    double m_beat = 0.0;
    double m_loop_start = 0.0;
    double m_loop_end = 0.0;
    double m_last_start = 0.0;
    uint64_t m_now = 0;
    bool m_playing = false;
    bool m_looping = false;
};

}  // namespace sim
