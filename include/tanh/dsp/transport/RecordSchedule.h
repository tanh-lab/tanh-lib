#pragma once

#include <tanh/core/AbiNamespace.h>
#include <tanh/core/threading/SeqLock.h>
#include <tanh/dsp/transport/TransportInfo.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace thl::inline THL_ABI_NAMESPACE::dsp::transport {

/// Where a RecordSchedule stands.
enum class RecordPhase : uint8_t {
    Idle,       ///< Nothing armed, nothing recording.
    Armed,      ///< Waiting for the next bar line to start.
    Recording,  ///< The take is running.
    Finishing,  ///< Stop was asked for: the take runs to the end of its bar.
    Tail        ///< The musical length is over; the tail (reverb, delay) is still recorded.
};

/// What a block contributes to the take: the samples [m_begin, m_end).
struct RecordWindow {
    uint32_t m_begin = 0;
    uint32_t m_end = 0;
    bool m_started = false;  ///< The take starts at m_begin.
    bool m_final = false;    ///< The take ends at m_end.

    /// True when no sample of the block belongs to the take.
    [[nodiscard]] constexpr bool empty() const noexcept { return m_end == m_begin; }
};

/// The musical facts of a take: valid from the block it started in, complete in the block
/// it ended in. The fields are those of audio_io::TakeTiming.
struct RecordTiming {
    double m_bpm = 0.0;            ///< Tempo at the start.
    int32_t m_sig_num = 4;         ///< Time signature at the start.
    int32_t m_sig_denom = 4;       ///< Time signature at the start.
    double m_start_beat = 0.0;     ///< Beat of the first sample: a bar line on the grid.
    double m_length_beats = 0.0;   ///< Musical length; 0 for a take off the grid.
    uint64_t m_length_frames = 0;  ///< Samples of the musical length (without the tail).
    uint64_t m_total_frames = 0;   ///< Samples of the take, tail included.
};

/// What a display needs while a take is prepared, running and ending. Beats are quarter
/// notes; the *_bars() helpers divide by the bar of the time signature.
struct RecordStatus {
    RecordPhase m_phase = RecordPhase::Idle;
    bool m_on_grid = false;            ///< The take starts and ends on bar lines.
    double m_beats_per_bar = 4.0;      ///< Bar of the current time signature.
    double m_beats_until_start = 0.0;  ///< Armed: beats to the bar line the take starts on.
    double m_elapsed_beats = 0.0;      ///< Musical time recorded so far (on the grid).
    double m_elapsed_seconds = 0.0;    ///< Time recorded so far, tail included.
    double m_length_beats = 0.0;       ///< The length the take will have, once known; else 0.
    double m_beats_left = 0.0;         ///< Beats to the end of that length.
    uint32_t m_takes_finished = 0;     ///< Counts up when a take ends: a change means "done".

    /// Bars recorded so far.
    [[nodiscard]] constexpr double elapsed_bars() const noexcept {
        return m_beats_per_bar > 0.0 ? m_elapsed_beats / m_beats_per_bar : 0.0;
    }
    /// The take's length in bars, once known; else 0.
    [[nodiscard]] constexpr double length_bars() const noexcept {
        return m_beats_per_bar > 0.0 ? m_length_beats / m_beats_per_bar : 0.0;
    }
};

/**
 * Decides, block by block, which samples belong to a recording, so that every take is a
 * whole number of bars: it starts on a bar line and ends on one, at the exact sample.
 *
 * - arm() makes it wait for the next bar line; the take starts there.
 * - stop() lets the take run to the end of its bar. arm(n) instead ends it after n bars by
 *   itself (on the grid only: off it, a take runs until stop()).
 * - A tail (set_tail_seconds()) keeps recording after the last bar, so reverb and delay ring
 *   out. The take's timing keeps the musical length apart from the tail, so the take still
 *   loops.
 * - status() says where things stand, for a count-in display, a bar counter and a "done".
 *
 * Bars are counted in beats of the transport, never in samples, so a tempo change during the
 * take still gives whole bars, and a host loop that wraps or a session that is joined does
 * not disturb the count.
 *
 * Off the grid: when the transport is not running as arm() takes effect (or with
 * set_grid_enabled(false)), the take starts at once, stop() ends it at once, and it has no
 * musical length. A take on the grid ends where the transport stops.
 *
 * The schedule only decides; it records nothing. Feed its window to a recorder:
 * @code
 * const auto window = schedule.advance(transport);
 * recorder.begin_block(transport.m_num_samples,
 *                      {.m_begin = window.m_begin, .m_end = window.m_end, .m_final =
 * window.m_final});
 * // recorder.write(...) for every track
 * if (window.m_final) {
 *     const auto& t = schedule.timing();
 *     recorder.set_timing({t.m_bpm, t.m_sig_num, t.m_sig_denom,
 *                          t.m_start_beat, t.m_length_beats, t.m_length_frames});
 * }
 * recorder.end_block();
 * @endcode
 *
 * Threading: prepare() with the audio stopped; advance() and timing() on the audio thread
 * (no locks, no allocation); everything else on any thread.
 */
class RecordSchedule {
public:
    /// Sets the sample rate (for the tail and the seconds of the status) and goes to Idle.
    void prepare(double sample_rate) noexcept {
        m_sample_rate = sample_rate > 0.0 ? sample_rate : 48000.0;
        m_arm_request.store(0, std::memory_order_relaxed);
        m_stop_request.store(false, std::memory_order_relaxed);
        m_cancel_request.store(false, std::memory_order_relaxed);
        m_phase = RecordPhase::Idle;
        m_timing = {};
        m_frames = 0;
        m_takes_finished = 0;
        publish(RecordStatus{});
    }

    /// Starts a take at the next bar line, @p length_bars bars long; 0: until stop(). Ignored
    /// unless Idle.
    void arm(uint32_t length_bars = 0) noexcept {
        m_arm_request.store(length_bars == UINT32_MAX ? length_bars : length_bars + 1,
                            std::memory_order_relaxed);
    }

    /// Ends the take at the end of its bar (at once off the grid); an armed take that has not
    /// started is dropped.
    void stop() noexcept { m_stop_request.store(true, std::memory_order_relaxed); }

    /// Back to Idle at the next block, whatever was going on. The recorder is told
    /// separately (MultitrackRecorder::cancel_take()).
    void cancel() noexcept {
        // An arm() from before goes too; one that follows this call counts.
        m_arm_request.store(0, std::memory_order_relaxed);
        m_cancel_request.store(true, std::memory_order_relaxed);
    }

    /// Time recorded after the musical end of a take on the grid; 0 (the default): none.
    void set_tail_seconds(double seconds) noexcept {
        m_tail_seconds.store(seconds > 0.0 ? seconds : 0.0, std::memory_order_relaxed);
    }

    /// False: takes start and stop at once even while the transport runs. Default: true.
    void set_grid_enabled(bool enabled) noexcept {
        m_grid_enabled.store(enabled, std::memory_order_relaxed);
    }

    /// Audio thread, once per block: which samples of this block belong to the take.
    RecordWindow advance(const TransportInfo& transport) noexcept TANH_NONBLOCKING_FUNCTION {
        const uint32_t num_samples = transport.m_num_samples;
        RecordWindow window;

        if (m_cancel_request.exchange(false, std::memory_order_relaxed)) {
            m_phase = RecordPhase::Idle;
        }
        const uint32_t arm_request = m_arm_request.exchange(0, std::memory_order_relaxed);
        if (arm_request != 0 && m_phase == RecordPhase::Idle) {
            m_phase = RecordPhase::Armed;
            m_target_bars = arm_request - 1;
        }
        bool stop = m_stop_request.exchange(false, std::memory_order_relaxed);
        if (stop && m_phase == RecordPhase::Armed) {
            m_phase = RecordPhase::Idle;
            stop = false;
        }

        const bool running = transport.is_playing() &&
                             transport.has(TransportInfo::k_has_beat_position) &&
                             transport.m_beats_per_sample > 0.0;
        const double bar =
            beats_per_division(Division::Bar, transport.m_sig_num, transport.m_sig_denom);
        double beats_until_start = 0.0;
        uint32_t cursor = 0;  // first sample of the block that is not decided yet

        if (m_phase == RecordPhase::Armed) {
            const bool on_grid =
                running && bar > 0.0 && m_grid_enabled.load(std::memory_order_relaxed);
            if (!on_grid) {
                start_take(transport, false, transport.m_beat_position, bar);
                window.m_started = true;
            } else {
                const double line = std::ceil((transport.m_beat_position / bar) - 1e-9) * bar;
                const double offset = sample_of(transport, line);
                if (offset < static_cast<double>(num_samples)) {
                    cursor = static_cast<uint32_t>(offset);
                    start_take(transport, true, line, bar);
                    window.m_started = true;
                } else {
                    beats_until_start = std::max(0.0, line - transport.beat_end());
                }
            }
            window.m_begin = cursor;
            window.m_end = cursor;
        }

        if (m_phase == RecordPhase::Recording || m_phase == RecordPhase::Finishing) {
            if (!m_on_grid) {
                if (stop && !window.m_started) {
                    finish_take(window);
                } else {
                    extend(window, num_samples);
                }
            } else {
                advance_on_grid(transport, window, running, stop, bar, cursor, num_samples);
            }
        } else if (m_phase == RecordPhase::Tail) {
            record_tail(window, 0, num_samples);
        }

        RecordStatus status;
        status.m_phase = m_phase;
        status.m_on_grid = m_on_grid && m_phase != RecordPhase::Idle;
        status.m_beats_per_bar = bar;
        status.m_beats_until_start = beats_until_start;
        status.m_elapsed_seconds = static_cast<double>(m_frames) / m_sample_rate;
        status.m_takes_finished = m_takes_finished;
        if (m_on_grid && m_phase != RecordPhase::Idle && m_phase != RecordPhase::Armed) {
            const double now = transport.beat_at(window.m_end);
            const double end = m_has_end ? m_end_beat : now;
            status.m_elapsed_beats = std::max(0.0, std::min(now, end) - m_origin_beat);
            status.m_length_beats = m_has_end ? m_end_beat - m_origin_beat : 0.0;
            status.m_beats_left = m_has_end ? std::max(0.0, m_end_beat - now) : 0.0;
        }
        publish(status);
        return window;
    }

    /// Audio thread: the timing of the take that is running, or of the one that just ended.
    [[nodiscard]] const RecordTiming& timing() const noexcept TANH_NONBLOCKING_FUNCTION {
        return m_timing;
    }

    /// Any thread: where the schedule stands, as of the end of the last block.
    [[nodiscard]] RecordStatus status() const noexcept {
        RecordStatus status;
        for (int attempt = 0; attempt < k_read_attempts; ++attempt) {
            const auto token = m_lock.read_begin();
            status.m_phase = m_status_phase.load(std::memory_order_relaxed);
            status.m_on_grid = m_status_on_grid.load(std::memory_order_relaxed);
            status.m_beats_per_bar = m_status_beats_per_bar.load(std::memory_order_relaxed);
            status.m_beats_until_start = m_status_until_start.load(std::memory_order_relaxed);
            status.m_elapsed_beats = m_status_elapsed_beats.load(std::memory_order_relaxed);
            status.m_elapsed_seconds = m_status_elapsed_seconds.load(std::memory_order_relaxed);
            status.m_length_beats = m_status_length_beats.load(std::memory_order_relaxed);
            status.m_beats_left = m_status_beats_left.load(std::memory_order_relaxed);
            status.m_takes_finished = m_status_takes.load(std::memory_order_relaxed);
            if (token && m_lock.read_valid(*token)) { break; }
        }
        return status;
    }

    /// Any thread: the phase alone.
    [[nodiscard]] RecordPhase phase() const noexcept {
        return m_status_phase.load(std::memory_order_relaxed);
    }

private:
    static constexpr int k_read_attempts = 64;

    /// The sample of the block at which the transport reaches @p beat (never negative; past
    /// the block when the beat lies beyond it). A beat the arithmetic misses by a hair still
    /// lands on its sample.
    [[nodiscard]] static double sample_of(const TransportInfo& transport,
                                          double beat) noexcept TANH_NONBLOCKING_FUNCTION {
        const double samples = (beat - transport.m_beat_position) / transport.m_beats_per_sample;
        return std::max(0.0, std::ceil(samples - 1e-6));
    }

    void start_take(const TransportInfo& transport,
                    bool on_grid,
                    double start_beat,
                    double bar) noexcept TANH_NONBLOCKING_FUNCTION {
        m_phase = RecordPhase::Recording;
        m_on_grid = on_grid;
        m_origin_beat = start_beat;
        m_frames = 0;
        m_has_end = on_grid && m_target_bars > 0;
        m_end_beat = start_beat + (static_cast<double>(m_target_bars) * bar);
        m_timing = RecordTiming{.m_bpm = transport.m_bpm,
                                .m_sig_num = transport.m_sig_num,
                                .m_sig_denom = transport.m_sig_denom,
                                .m_start_beat = start_beat};
    }

    void extend(RecordWindow& window, uint32_t end) noexcept TANH_NONBLOCKING_FUNCTION {
        m_frames += end - window.m_end;
        window.m_end = end;
    }

    void finish_take(RecordWindow& window) noexcept TANH_NONBLOCKING_FUNCTION {
        m_timing.m_total_frames = m_frames;
        m_phase = RecordPhase::Idle;
        ++m_takes_finished;
        window.m_final = true;
    }

    void record_tail(RecordWindow& window,
                     uint32_t from,
                     uint32_t num_samples) noexcept TANH_NONBLOCKING_FUNCTION {
        const auto room = static_cast<uint64_t>(num_samples - from);
        const auto taken = static_cast<uint32_t>(std::min<uint64_t>(room, m_tail_left));
        window.m_end = from;
        if (window.empty()) { window.m_begin = from; }
        extend(window, from + taken);
        m_tail_left -= taken;
        if (m_tail_left == 0) { finish_take(window); }
    }

    void advance_on_grid(const TransportInfo& transport,
                         RecordWindow& window,
                         bool running,
                         bool stop,
                         double bar,
                         uint32_t cursor,
                         uint32_t num_samples) noexcept TANH_NONBLOCKING_FUNCTION {
        if (!window.m_started) {
            if (!running) {
                // The transport stopped under the take: it ends here, wherever in the bar.
                m_timing.m_length_beats = std::max(0.0, m_last_beat - m_origin_beat);
                m_timing.m_length_frames = m_frames;
                finish_take(window);
                return;
            }
            // A seek, a loop wrap or a timeline that starts over (a shared session joined)
            // moves the beat, not the take: its bars go on being counted from where it is.
            if (transport.has(TransportInfo::k_jumped | TransportInfo::k_timeline_reset)) {
                const double delta = transport.has(TransportInfo::k_jumped)
                                         ? transport.m_jump_delta_beats
                                         : transport.m_beat_position - m_last_beat;
                m_origin_beat += delta;
                m_end_beat += delta;
            }
        }
        m_last_beat = transport.beat_end();

        if (stop && bar > 0.0) {
            const double elapsed = transport.beat_at(cursor) - m_origin_beat;
            const double bars = std::max(1.0, std::ceil((elapsed / bar) - 1e-9));
            const double end = m_origin_beat + (bars * bar);
            m_end_beat = m_has_end ? std::min(m_end_beat, end) : end;
            m_has_end = true;
            m_phase = RecordPhase::Finishing;
        }

        const double end_sample =
            m_has_end ? std::max(sample_of(transport, m_end_beat), static_cast<double>(cursor))
                      : static_cast<double>(num_samples) + 1.0;
        if (end_sample > static_cast<double>(num_samples)) {
            extend(window, num_samples);
            return;
        }

        // The last bar line of the take is in this block.
        const auto end = static_cast<uint32_t>(end_sample);
        extend(window, end);
        m_timing.m_length_beats = m_end_beat - m_origin_beat;
        m_timing.m_length_frames = m_frames;

        const double tail = m_tail_seconds.load(std::memory_order_relaxed) * m_sample_rate;
        m_tail_left = static_cast<uint64_t>(std::max(0.0, std::round(tail)));
        if (m_tail_left == 0) {
            finish_take(window);
            return;
        }
        m_phase = RecordPhase::Tail;
        record_tail(window, end, num_samples);
    }

    void publish(const RecordStatus& status) noexcept TANH_NONBLOCKING_FUNCTION {
        m_lock.write_begin();
        m_status_phase.store(status.m_phase, std::memory_order_relaxed);
        m_status_on_grid.store(status.m_on_grid, std::memory_order_relaxed);
        m_status_beats_per_bar.store(status.m_beats_per_bar, std::memory_order_relaxed);
        m_status_until_start.store(status.m_beats_until_start, std::memory_order_relaxed);
        m_status_elapsed_beats.store(status.m_elapsed_beats, std::memory_order_relaxed);
        m_status_elapsed_seconds.store(status.m_elapsed_seconds, std::memory_order_relaxed);
        m_status_length_beats.store(status.m_length_beats, std::memory_order_relaxed);
        m_status_beats_left.store(status.m_beats_left, std::memory_order_relaxed);
        m_status_takes.store(status.m_takes_finished, std::memory_order_relaxed);
        m_lock.write_end();
    }

    // Requests, from any thread.
    std::atomic<uint32_t> m_arm_request{0};  // 0: none; else the length in bars + 1
    std::atomic<bool> m_stop_request{false};
    std::atomic<bool> m_cancel_request{false};
    std::atomic<double> m_tail_seconds{0.0};
    std::atomic<bool> m_grid_enabled{true};

    // Audio thread.
    double m_sample_rate = 48000.0;
    RecordPhase m_phase = RecordPhase::Idle;
    bool m_on_grid = false;
    bool m_has_end = false;      // the beat the musical length ends on is known
    uint32_t m_target_bars = 0;  // length asked for with arm(); 0: open
    double m_origin_beat = 0.0;  // the take's first beat, in the transport's current timeline
    double m_end_beat = 0.0;
    double m_last_beat = 0.0;  // beat at the end of the last block of the take
    uint64_t m_frames = 0;
    uint64_t m_tail_left = 0;
    uint32_t m_takes_finished = 0;
    RecordTiming m_timing;

    // What status() reads: written by the audio thread under m_lock.
    SeqLock m_lock;
    std::atomic<RecordPhase> m_status_phase{RecordPhase::Idle};
    std::atomic<bool> m_status_on_grid{false};
    std::atomic<double> m_status_beats_per_bar{4.0};
    std::atomic<double> m_status_until_start{0.0};
    std::atomic<double> m_status_elapsed_beats{0.0};
    std::atomic<double> m_status_elapsed_seconds{0.0};
    std::atomic<double> m_status_length_beats{0.0};
    std::atomic<double> m_status_beats_left{0.0};
    std::atomic<uint32_t> m_status_takes{0};
};

}  // namespace thl::inline THL_ABI_NAMESPACE::dsp::transport
