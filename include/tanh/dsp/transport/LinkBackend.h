#pragma once

#include <tanh/utils/RealtimeSanitizer.h>

#include <cstdint>

namespace thl::dsp::transport {

/// Link session state captured for one audio block.
struct LinkState {
    double m_tempo = 120.0;
    bool m_playing = false;
    int64_t m_play_time_us = 0;  ///< host time of the last start/stop (timeForIsPlaying)
};

/**
 * @brief Audio-thread seam between LinkTransportClock and an Ableton Link SDK.
 *
 * Mirrors the audio-thread subset of Ableton Link's API (captureAudioSessionState,
 * the SessionState queries/requests, commitAudioSessionState). Times are host
 * times in microseconds on the Link clock. Implementations: the C++ Link SDK on
 * desktop and LinkKit on iOS (thl::link::LinkSession::audio_backend(), built with
 * TANH_WITH_LINK), and a scripted fake in the tests.
 *
 * Every method except timeline_epoch() is called only from the audio thread,
 * between capture() and commit() for the session-state calls. All must be
 * real-time safe (Link documents its audio-thread calls as such).
 */
class LinkBackend {
public:
    virtual ~LinkBackend() = default;

    /// captureAudioSessionState()
    virtual void capture() noexcept TANH_NONBLOCKING_FUNCTION = 0;
    /// beatAtTime(t, quantum) of the captured state
    [[nodiscard]] virtual double beat_at(int64_t t_us, double quantum) const noexcept
        TANH_NONBLOCKING_FUNCTION = 0;
    /// tempo(), isPlaying(), timeForIsPlaying() of the captured state
    [[nodiscard]] virtual LinkState state() const noexcept TANH_NONBLOCKING_FUNCTION = 0;
    /// setTempo(bpm, t)
    virtual void set_tempo(double bpm, int64_t t_us) noexcept TANH_NONBLOCKING_FUNCTION = 0;
    /// setIsPlaying(playing, t)
    virtual void set_playing(bool playing, int64_t t_us) noexcept TANH_NONBLOCKING_FUNCTION = 0;
    /// requestBeatAtStartPlayingTime(beat, quantum)
    virtual void request_beat_at_start(double beat,
                                       double quantum) noexcept TANH_NONBLOCKING_FUNCTION = 0;
    /// requestBeatAtTime(beat, t, quantum)
    virtual void request_beat_at(double beat,
                                 int64_t t_us,
                                 double quantum) noexcept TANH_NONBLOCKING_FUNCTION = 0;
    /// commitAudioSessionState() of the captured (and modified) state
    virtual void commit() noexcept TANH_NONBLOCKING_FUNCTION = 0;
    /// The Link clock's current host time in microseconds.
    [[nodiscard]] virtual int64_t now_us() const noexcept TANH_NONBLOCKING_FUNCTION = 0;

    /**
     * @brief Bumped (any thread) when Link is enabled or the first peer joins.
     *
     * The clock reports k_timeline_reset (with k_jumped) when the epoch changed
     * and the beat actually moved; enabling Link alone moves nothing (BEATTIME-1).
     */
    [[nodiscard]] virtual uint64_t timeline_epoch() const noexcept TANH_NONBLOCKING_FUNCTION {
        return 0;
    }
};

}  // namespace thl::dsp::transport
