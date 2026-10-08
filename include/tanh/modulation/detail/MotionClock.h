#pragma once

#include <tanh/dsp/transport/TransportInfo.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <cstdint>

namespace thl::modulation::detail {

/// The beats a MotionRecorder block runs on.
struct MotionBlockClock {
    double m_bpm = 120.0;
    double m_beat = 0.0;       ///< beat at the block's first sample
    double m_slope = 0.0;      ///< playback beats per sample (0 while held)
    double m_rec_slope = 0.0;  ///< recording beats per sample: never stops
    bool m_playing = false;    ///< the transport plays
    bool m_jumped = false;     ///< k_jumped or k_started
    bool m_timeline_reset = false;

    [[nodiscard]] double beat_at(uint32_t offset) const {
        return m_beat + (static_cast<double>(offset) * m_slope);
    }
};

/**
 * @brief The transport's beat while it plays, else the recorder's own free-run.
 *
 * Stopped, the beat continues from the previous block's end at the last tempo
 * (or holds); a timeline reset restarts the free-run from the transport's beat.
 * The transport's discontinuity flags are passed on as the only jump detector.
 */
class MotionClock {
public:
    /// Starts over: the next block takes the transport's beat.
    void reset() TANH_NONBLOCKING_FUNCTION { m_have_end = false; }

    /// The clock of a block of @p transport at @p sample_rate; @p hold freezes it while stopped.
    [[nodiscard]] MotionBlockClock begin(const dsp::transport::TransportInfo& transport,
                                         double sample_rate,
                                         bool hold) TANH_NONBLOCKING_FUNCTION {
        using dsp::transport::TransportInfo;
        MotionBlockClock c;
        c.m_bpm = transport.m_bpm > 0.0 ? transport.m_bpm : 120.0;
        const double nominal = c.m_bpm / (60.0 * sample_rate);
        c.m_playing = transport.is_playing();
        c.m_timeline_reset = transport.has(TransportInfo::k_timeline_reset);
        c.m_jumped =
            transport.has(TransportInfo::k_jumped) || transport.has(TransportInfo::k_started);
        if (c.m_timeline_reset) { m_have_end = false; }
        c.m_beat = transport.m_beat_position;
        c.m_slope = nominal;
        if (c.m_playing) {
            if (transport.m_beats_per_sample > 0.0) { c.m_slope = transport.m_beats_per_sample; }
        } else {
            if (m_have_end) { c.m_beat = m_end; }
            c.m_slope = hold ? 0.0 : nominal;
        }
        c.m_rec_slope = c.m_playing && transport.m_beats_per_sample > 0.0
                            ? transport.m_beats_per_sample
                            : nominal;
        m_have_end = true;
        return c;
    }

    /// Ends the block of @p num_samples that @p clock started.
    void end(const MotionBlockClock& clock, uint32_t num_samples) TANH_NONBLOCKING_FUNCTION {
        m_end = clock.beat_at(num_samples);
    }

private:
    bool m_have_end = false;
    double m_end = 0.0;  // beat at the end of the previous block
};

}  // namespace thl::modulation::detail
