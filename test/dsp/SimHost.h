#pragma once

// Deterministic simulated host playhead for transport tests.
//
// A script of sample-stamped events (seek, shift, tempo, play, stop, loop) drives a
// host the way a DAW does: changes requested anywhere inside a block become
// visible at the next block boundary (hosts report one position per block), a
// cycle wraps at the first block start past the loop end, and the beat moves
// at the current tempo only while playing. next_block() returns the host's
// per-block TransportInfo (validity flags, no discontinuity bits) — what a
// JUCE/CLAP/VST3 adapter would hand to HostTransportClock::set_host_info().
//
// TempoAt and TempoRamp model the DAW's tempo map instead: they take effect at
// their exact sample, also inside a block. The host still reports one bpm per
// block (the tempo at the block start, as JUCE's PositionInfo does), but its beat
// integrates the tempo map per sample, so the next block's position differs from
// "start + frames * reported bpm" by what the tempo change added inside the block.

#include <tanh/dsp/transport/TransportInfo.h>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace sim {

struct Event {
    enum class Kind {
        Seek,
        Shift,
        Tempo,  ///< m_a = bpm, from the next block boundary
        Play,
        Stop,
        Loop,
        LoopOff,
        TempoAt,   ///< tempo map: m_a = bpm from exactly m_sample (also mid-block)
        TempoRamp  ///< tempo map: linear ramp to m_a bpm over m_b samples from m_sample
    };
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
        auto& list = is_tempo_map(e.m_kind) ? m_tempo_map : m_script;
        list.push_back(e);
        std::stable_sort(list.begin(), list.end(), [](const Event& a, const Event& b) {
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

        apply_tempo_map(m_now);

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
        const bool map_active =
            m_ramp_length > 0 ||
            (m_next_map < m_tempo_map.size() && m_tempo_map[m_next_map].m_sample < m_now + frames);
        if (map_active) {
            // Integrate the tempo map per sample (the tempo of sample i moves the beat
            // from i to i + 1), so the next block starts where a DAW would put it.
            for (uint32_t i = 0; i < frames; ++i) {
                apply_tempo_map(m_now + i);
                if (m_playing) { m_beat += bpm_at(m_now + i) / (60.0 * m_sample_rate); }
            }
            m_bpm = bpm_at(m_now + frames);
            if (m_ramp_length > 0 && m_now + frames >= m_ramp_start + m_ramp_length) {
                m_ramp_length = 0;  // ramp done: hold its target
            }
        } else if (m_playing) {
            m_beat += static_cast<double>(frames) * m_bpm / (60.0 * m_sample_rate);
        }
        m_now += frames;
        return info;
    }

    [[nodiscard]] uint64_t now() const { return m_now; }
    [[nodiscard]] double last_start() const { return m_last_start; }
    /// Beat the host will report at the start of the next block (its true position now).
    [[nodiscard]] double beat() const { return m_beat; }

private:
    static bool is_tempo_map(Event::Kind kind) {
        return kind == Event::Kind::TempoAt || kind == Event::Kind::TempoRamp;
    }

    // Apply tempo-map events stamped at or before @p sample.
    void apply_tempo_map(uint64_t sample) {
        while (m_next_map < m_tempo_map.size() && m_tempo_map[m_next_map].m_sample <= sample) {
            const Event& e = m_tempo_map[m_next_map++];
            m_bpm = bpm_at(e.m_sample);
            m_ramp_length = 0;
            if (e.m_kind == Event::Kind::TempoAt) {
                m_bpm = e.m_a;
            } else {
                m_ramp_start = e.m_sample;
                m_ramp_from = m_bpm;
                m_ramp_to = e.m_a;
                m_ramp_length = static_cast<uint64_t>(std::max(e.m_b, 1.0));
            }
        }
    }

    // Tempo of @p sample: the ramp's value while one runs, else the current bpm.
    [[nodiscard]] double bpm_at(uint64_t sample) const {
        if (m_ramp_length == 0) { return m_bpm; }
        const double t = std::clamp(
            static_cast<double>(sample - m_ramp_start) / static_cast<double>(m_ramp_length),
            0.0,
            1.0);
        return m_ramp_from + ((m_ramp_to - m_ramp_from) * t);
    }

    void apply(const Event& e) {
        switch (e.m_kind) {
            case Event::Kind::Seek: m_beat = e.m_a; break;
            case Event::Kind::Shift: m_beat += e.m_a; break;  // Link phase realignment
            case Event::Kind::Tempo:
                m_bpm = e.m_a;
                m_ramp_length = 0;
                break;
            case Event::Kind::Play: m_playing = true; break;
            case Event::Kind::Stop: m_playing = false; break;
            case Event::Kind::Loop:
                m_looping = true;
                m_loop_start = e.m_a;
                m_loop_end = e.m_b;
                break;
            case Event::Kind::LoopOff: m_looping = false; break;
            case Event::Kind::TempoAt:
            case Event::Kind::TempoRamp: break;  // tempo map: apply_tempo_map()
        }
    }

    std::vector<Event> m_script;
    size_t m_next = 0;
    std::vector<Event> m_tempo_map;
    size_t m_next_map = 0;
    uint64_t m_ramp_start = 0;
    uint64_t m_ramp_length = 0;  // 0: no ramp running
    double m_ramp_from = 0.0;
    double m_ramp_to = 0.0;
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
