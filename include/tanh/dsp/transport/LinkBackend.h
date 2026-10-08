#pragma once

#include <tanh/utils/RealtimeSanitizer.h>

#include <cstdint>

namespace thl::dsp::transport {

/// Link session state captured for one audio block.
struct LinkState {
    double m_tempo = 120.0;
    bool m_playing = false;
    int64_t m_play_time_us = 0;  ///< Host time of the last start or stop (timeForIsPlaying).
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
 * The clock calls every method from the audio thread only, the session-state
 * calls between capture() and commit(). All must be real-time safe, as Link
 * documents its audio-thread calls.
 */
class LinkBackend {
public:
    virtual ~LinkBackend() = default;

    /// Captures the audio session state (captureAudioSessionState).
    virtual void capture() TANH_NONBLOCKING_FUNCTION = 0;
    /// Beat at host time @p t_us of the captured state (beatAtTime).
    [[nodiscard]] virtual double beat_at(int64_t t_us,
                                         double quantum) const TANH_NONBLOCKING_FUNCTION = 0;
    /// Tempo and play state of the captured state.
    [[nodiscard]] virtual LinkState state() const TANH_NONBLOCKING_FUNCTION = 0;
    /// Sets the tempo from host time @p t_us (setTempo).
    virtual void set_tempo(double bpm, int64_t t_us) TANH_NONBLOCKING_FUNCTION = 0;
    /// Starts or stops at host time @p t_us (setIsPlaying).
    virtual void set_playing(bool playing, int64_t t_us) TANH_NONBLOCKING_FUNCTION = 0;
    /// Maps @p beat to the start time, quantized (requestBeatAtStartPlayingTime).
    virtual void request_beat_at_start(double beat, double quantum) TANH_NONBLOCKING_FUNCTION = 0;
    /// Maps @p beat to host time @p t_us, quantized (requestBeatAtTime).
    virtual void request_beat_at(double beat,
                                 int64_t t_us,
                                 double quantum) TANH_NONBLOCKING_FUNCTION = 0;
    /// Commits the captured and modified state (commitAudioSessionState).
    virtual void commit() TANH_NONBLOCKING_FUNCTION = 0;
    /// The Link clock's current host time in microseconds.
    [[nodiscard]] virtual int64_t now_us() const TANH_NONBLOCKING_FUNCTION = 0;

    /**
     * @brief Counter bumped (from any thread) when Link is enabled or the first peer joins.
     *
     * The clock adds k_timeline_reset to the first k_jumped shortly after a change.
     * Read from the audio thread before capture().
     */
    [[nodiscard]] virtual uint64_t timeline_epoch() const TANH_NONBLOCKING_FUNCTION { return 0; }
};

}  // namespace thl::dsp::transport
