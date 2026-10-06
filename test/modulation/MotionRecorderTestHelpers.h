#pragma once

#include <tanh/dsp/transport/HostTransportClock.h>
#include <tanh/dsp/transport/TransportInfo.h>
#include <tanh/modulation/MotionLane.h>
#include <tanh/modulation/MotionRecorder.h>
#include <tanh/modulation/XYController.h>
#include <tanh/modulation/detail/XYPad.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "../dsp/SimHost.h"

namespace motion_test {

using thl::dsp::transport::HostTransportClock;
using thl::dsp::transport::TransportInfo;

constexpr double k_sr = 48000.0;

/// A simulated host drives a HostTransportClock, whose per-block TransportInfo
/// (with real discontinuity flags) drives pad and recorder in the order
/// XYController uses. Every output sample is kept.
struct Rig {
    explicit Rig(uint32_t block,
                 double bpm = 120.0,
                 const thl::modulation::MotionRecorderConfig& cfg = {})
        : m_block(block), m_host(k_sr, bpm) {
        m_clock.prepare(k_sr);
        m_pad.prepare(4096);
        m_rec.prepare(k_sr, 4096, cfg);
    }

    void play_at(uint64_t sample) { m_host.add({sample, sim::Event::Kind::Play}); }
    void stop_at(uint64_t sample) { m_host.add({sample, sim::Event::Kind::Stop}); }
    void seek_at(uint64_t sample, double beat) {
        m_host.add({sample, sim::Event::Kind::Seek, beat});
    }
    void shift_at(uint64_t sample, double beats) {
        m_host.add({sample, sim::Event::Kind::Shift, beats});
    }
    /// Tempo change at the next block boundary (the host reports it exactly).
    void tempo_at(uint64_t sample, double bpm) {
        m_host.add({sample, sim::Event::Kind::Tempo, bpm});
    }
    /// Tempo-map step at exactly @p sample, also inside a block.
    void tempo_step_at(uint64_t sample, double bpm) {
        m_host.add({sample, sim::Event::Kind::TempoAt, bpm});
    }
    /// Tempo-map ramp from the current tempo to @p bpm over @p samples from @p sample.
    void tempo_ramp_at(uint64_t sample, double bpm, uint64_t samples) {
        m_host.add({sample, sim::Event::Kind::TempoRamp, bpm, static_cast<double>(samples)});
    }
    void loop_at(uint64_t sample, double start, double end) {
        m_host.add({sample, sim::Event::Kind::Loop, start, end});
    }

    void loop_off_at(uint64_t sample) { m_host.add({sample, sim::Event::Kind::LoopOff}); }

    /// One block of @p n samples (default: the rig's block size).
    TransportInfo step(uint32_t n = 0) {
        if (n == 0) { n = m_block; }
        m_clock.set_host_info(m_host.next_block(n));
        m_clock.begin_block(n);
        TransportInfo info = m_clock.block_info();
        info.m_flags |= m_extra_flags;
        m_extra_flags = 0;
        if (m_before_pad) { m_before_pad(info); }
        m_pad.process_block(n);
        m_rec.process(info, m_pad.output(), n);
        for (uint32_t i = 0; i < n; ++i) {
            m_x.push_back(m_rec.out_x()[i]);
            m_y.push_back(m_rec.out_y()[i]);
            m_gate.push_back(m_rec.out_gate()[i]);
            m_beat.push_back(info.beat_at(i));
        }
        for (const uint32_t cp : m_rec.change_points()) { m_cps.push_back(m_now + cp); }
        m_clock.end_block();
        m_now += n;
        return info;
    }

    /// Run whole blocks until at least @p sample samples were processed.
    void run_until(uint64_t sample) {
        while (m_now < sample) { step(); }
    }

    [[nodiscard]] uint64_t now() const { return m_now; }

    uint32_t m_block;
    sim::SimHost m_host;
    HostTransportClock m_clock;
    thl::modulation::detail::XYPad m_pad{1, thl::modulation::MonoPriority::Last};
    thl::modulation::MotionRecorder m_rec;
    uint32_t m_extra_flags = 0;  // OR'ed into the next block's TransportInfo (hints)
    /// Called with the block's TransportInfo before the pad drains its touches
    /// (a finger that follows the transport beat).
    std::function<void(const TransportInfo&)> m_before_pad;
    uint64_t m_now = 0;

    std::vector<float> m_x;
    std::vector<float> m_y;
    std::vector<uint8_t> m_gate;
    std::vector<double> m_beat;
    std::vector<uint64_t> m_cps;
};

/// A playing transport block at @p beat.
inline TransportInfo playing_info(double beat, uint32_t n, double bpm = 120.0) {
    TransportInfo t;
    t.m_flags = TransportInfo::k_is_playing | TransportInfo::k_has_tempo |
                TransportInfo::k_has_beat_position;
    t.m_bpm = bpm;
    t.m_beat_position = beat;
    t.m_beats_per_sample = bpm / (60.0 * k_sr);
    t.m_num_samples = n;
    return t;
}

/// Largest |Δ| between consecutive render ticks (every @p interval samples) in [from, to).
inline double max_tick_step(const std::vector<float>& v,
                            size_t from,
                            size_t to,
                            size_t interval = 32) {
    double m = 0.0;
    for (size_t i = from + interval; i < to && i < v.size(); i += interval) {
        m = std::max(m, static_cast<double>(std::abs(v[i] - v[i - interval])));
    }
    return m;
}

/// Largest |Δ| between consecutive samples in [from, to).
inline double max_sample_step(const std::vector<float>& v, size_t from, size_t to) {
    double m = 0.0;
    for (size_t i = std::max<size_t>(from, 1); i < to && i < v.size(); ++i) {
        m = std::max(m, static_cast<double>(std::abs(v[i] - v[i - 1])));
    }
    return m;
}

inline bool all_in_unit_range(const std::vector<float>& v) {
    return std::all_of(v.begin(), v.end(), [](float f) {
        return std::isfinite(f) && f >= 0.0f && f <= 1.0f;
    });
}

/// A Beats lane of @p beats beats at @p tpb points per beat: x is a sine, y a
/// cosine over one loop, gate open.
inline thl::modulation::MotionLane beats_lane(double beats, uint32_t tpb, double anchor = 0.0) {
    thl::modulation::MotionLane lane;
    lane.m_timebase = thl::modulation::MotionTimebase::Beats;
    lane.m_rate = tpb;
    lane.m_length = beats;
    lane.m_anchor = anchor;
    const auto n = static_cast<size_t>(std::lround(beats * tpb));
    for (size_t i = 0; i < n; ++i) {
        const double ph =
            2.0 * 3.14159265358979323846 * static_cast<double>(i) / static_cast<double>(n);
        lane.m_x.push_back(static_cast<float>(0.5 + (0.4 * std::sin(ph))));
        lane.m_y.push_back(static_cast<float>(0.5 + (0.4 * std::cos(ph))));
        lane.m_gate.push_back(1);
    }
    return lane;
}

/// Phase (in loop units) that maps to @p value of beats_lane()'s x on its rising half.
inline double sine_phase_of(float x, float y, double length) {
    const double ph = std::atan2((x - 0.5) / 0.4, (y - 0.5) / 0.4);  // sin, cos
    double t = ph / (2.0 * 3.14159265358979323846);
    if (t < 0.0) { t += 1.0; }
    return t * length;
}

}  // namespace motion_test
