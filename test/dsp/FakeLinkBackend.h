#pragma once

// Deterministic stand-in for an Ableton Link session (test only).
//
// Models the parts of Link the clock relies on: a timeline beat(t) =
// ref_beat + (t - ref_time) * tempo / 60e6 that stays continuous across tempo
// changes, start/stop state with a start time, capture/commit of an audio
// session state, and request/force-beat-at-time remapping. Peer actions
// (tempo change, phase realignment on join, start/stop) are applied to the
// committed session between blocks, as Link's network thread would.

#include <tanh/dsp/transport/LinkBackend.h>

#include <cmath>
#include <cstdint>

namespace fake {

struct Session {
    double m_tempo = 120.0;
    double m_ref_beat = 0.0;
    int64_t m_ref_time = 0;
    bool m_playing = false;
    int64_t m_play_time = 0;

    [[nodiscard]] double beat_at(int64_t t) const {
        return m_ref_beat + static_cast<double>(t - m_ref_time) * m_tempo / 60e6;
    }
    void set_tempo(double bpm, int64_t t) {
        m_ref_beat = beat_at(t);
        m_ref_time = t;
        m_tempo = bpm;
    }
    void force_beat_at(double beat, int64_t t) {
        m_ref_beat = beat;
        m_ref_time = t;
    }
};

class FakeLinkBackend final : public thl::dsp::transport::LinkBackend {
public:
    // ── audio-thread seam ──────────────────────────────────────────────────
    void capture() noexcept override { m_captured = m_session; }
    [[nodiscard]] double beat_at(int64_t t_us, double /*quantum*/) const noexcept override {
        return m_captured.beat_at(t_us);
    }
    [[nodiscard]] thl::dsp::transport::LinkState state() const noexcept override {
        return {.m_tempo = m_captured.m_tempo,
                .m_playing = m_captured.m_playing,
                .m_play_time_us = m_captured.m_play_time};
    }
    void set_tempo(double bpm, int64_t t_us) noexcept override {
        ++m_set_tempo_calls;
        m_captured.set_tempo(bpm, t_us);
    }
    void set_playing(bool playing, int64_t t_us) noexcept override {
        m_captured.m_playing = playing;
        m_captured.m_play_time = t_us;
    }
    void request_beat_at_start(double beat, double quantum) noexcept override {
        ++m_request_start_calls;
        if (m_captured.m_playing) { request_beat_at(beat, m_captured.m_play_time, quantum); }
    }
    void request_beat_at(double beat, int64_t t_us, double quantum) noexcept override {
        if (!m_has_peers) {
            m_captured.force_beat_at(beat, t_us);  // alone: exact remap
            return;
        }
        // With peers only the local beat moves by whole quanta (phase is shared).
        const double current = m_captured.beat_at(t_us);
        const double shift = std::round((beat - current) / quantum) * quantum;
        m_captured.force_beat_at(current + shift, t_us);
    }
    void commit() noexcept override {
        ++m_commits;
        m_session = m_captured;
    }
    [[nodiscard]] int64_t now_us() const noexcept override { return m_now; }
    [[nodiscard]] uint64_t timeline_epoch() const noexcept override { return m_epoch; }

    // ── scripted "network" side ────────────────────────────────────────────
    void peer_set_tempo(double bpm, int64_t t) { m_session.set_tempo(bpm, t); }
    /// A peer joins and Link realigns our phase: the beat at t moves by delta.
    void peer_join_realign(double delta, int64_t t) {
        m_has_peers = true;
        ++m_epoch;
        m_session.force_beat_at(m_session.beat_at(t) + delta, t);
    }
    void enable_without_peers() { ++m_epoch; }
    /// A peer starts with start/stop sync and maps beat 0 to its start time.
    void peer_start(int64_t play_time) {
        m_session.m_playing = true;
        m_session.m_play_time = play_time;
        m_session.force_beat_at(0.0, play_time);
    }
    void peer_stop(int64_t t) {
        m_session.m_playing = false;
        m_session.m_play_time = t;
    }

    Session m_session;
    Session m_captured;
    int64_t m_now = 0;
    uint64_t m_epoch = 0;
    bool m_has_peers = false;
    int m_set_tempo_calls = 0;
    int m_request_start_calls = 0;
    int m_commits = 0;
};

}  // namespace fake
