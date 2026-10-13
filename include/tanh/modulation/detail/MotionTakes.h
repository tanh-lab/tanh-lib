#pragma once

#include <tanh/core/Exports.h>
#include <tanh/dsp/transport/TransportInfo.h>
#include <tanh/modulation/MotionLane.h>
#include <tanh/modulation/MotionLaneProcessing.h>
#include <tanh/modulation/MotionTypes.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace thl::modulation::detail {

/// A take buffer's place in the handoff between the audio thread and service().
enum class TakeBufferState : uint8_t { Free, Recording, Finished, Published };

/// One preallocated take: written by the audio thread, published by service().
struct TakeBuffer {
    std::atomic<TakeBufferState> m_state{TakeBufferState::Free};
    std::vector<float> m_x;
    std::vector<float> m_y;
    std::vector<uint8_t> m_gate;
    // Raw values of the points the seam closing blended, for service() to restore.
    std::vector<float> m_raw_tail_x;
    std::vector<float> m_raw_tail_y;
    // Written by the audio thread before Finished (release), read after (acquire).
    size_t m_num_points = 0;
    MotionTimebase m_timebase = MotionTimebase::Seconds;
    double m_rate = 0.0;
    double m_length = 0.0;
    double m_anchor = 0.0;
    uint32_t m_take_id = 0;
    size_t m_seam = 0;         // index of the first recorded point
    size_t m_seam_window = 0;  // points blended by the seam closing
};

/// What played when an overwriting take starts (a Beats loop): the take starts as it.
struct TakeSeed {
    const float* m_x = nullptr;
    const float* m_y = nullptr;
    const uint8_t* m_gate = nullptr;  ///< null: all down
    size_t m_num_points = 0;
    double m_length = 0.0;  ///< beats
    double m_anchor = 0.0;  ///< beat of point 0
    [[nodiscard]] bool empty() const { return m_num_points == 0 || !(m_length > 0.0); }
};

/**
 * @brief Records takes into two preallocated buffers on the audio thread.
 *
 * Owns the take that is running and the two TakeBuffers. start() opens a take,
 * step() advances it by one sample, end() ends it on disarm() / stop(). A take
 * that finishes is finalised in its buffer (TakeBufferState::Finished, a new id
 * from the shared counter); MotionRecorder plays it from there and service()
 * publishes it. Real-time safe after prepare().
 */
class TANH_API MotionTakeWriter {
public:
    enum class Start : uint8_t {
        Started,
        Busy,    ///< both buffers wait for service(); try again
        Refused  ///< the loop does not fit at the minimum resolution
    };

    /// What a step() or end() did to the running take.
    enum class Outcome : uint8_t {
        Running,    ///< still recording (or no take)
        Finished,   ///< finished into finished_buffer(): play it
        Dropped,    ///< too short or incomplete: nothing kept, the lane it replaced is gone
        Cancelled,  ///< never touched: nothing kept, the lane it replaced plays on
    };

    explicit MotionTakeWriter(std::atomic<uint32_t>& next_take_id) : m_next_take_id(next_take_id) {}

    /// Allocate the buffers and forget any take (message thread, audio stopped).
    void prepare(const MotionRecorderConfig& config, double sample_rate);
    [[nodiscard]] size_t buffer_bytes() const;

    /// Seam closing of finished takes (LoopEnd::Smooth closes it in the buffer).
    void set_loop_end(LoopEnd loop_end) TANH_NONBLOCKING_FUNCTION { m_loop_end = loop_end; }

    /**
     * @brief Opens a take of @p length at @p clock_beat (the recording clock's beat
     *        at this sample), starting from (@p first_x, @p first_y). An overwriting
     *        take (MotionRecorderConfig::m_overwrite_loops) starts as @p seed.
     */
    [[nodiscard]] Start start(const dsp::transport::TransportInfo& transport,
                              LoopLength length,
                              float first_x,
                              float first_y,
                              double clock_beat,
                              const TakeSeed& seed = {}) TANH_NONBLOCKING_FUNCTION;

    /**
     * @brief One sample of the running take.
     *
     * @param live       a finger is down this sample, at (@p x, @p y)
     * @param rec_slope  the recording clock's beats per sample
     * @param block_samples the block length (the still tail a free take may drop)
     */
    [[nodiscard]] Outcome step(bool live,
                               float x,
                               float y,
                               double rec_slope,
                               double bpm,
                               uint32_t block_samples) TANH_NONBLOCKING_FUNCTION;

    /// disarm() / stop() during a take: a touched bar-aligned BarsN take is padded to its
    /// loop (an overwriting one keeps what it holds), a free one ends where it is; other
    /// takes are dropped or cancelled.
    [[nodiscard]] Outcome end(double bpm) TANH_NONBLOCKING_FUNCTION;

    /// Drops the running take, keeping nothing (prepare() during a take).
    void abort() TANH_NONBLOCKING_FUNCTION;

    [[nodiscard]] bool active() const { return m_take.m_active; }
    /// An overwriting take whose whole loop holds motion: it plays from its buffer
    /// (the running take's) while it records.
    [[nodiscard]] bool playable() const {
        return m_take.m_active && m_take.m_overwrite && m_take.m_full;
    }
    /// The running take's buffer.
    [[nodiscard]] uint32_t buffer() const { return m_take.m_buffer; }
    /// Recording progress, 0..1 of the current loop target; 0 without a take.
    [[nodiscard]] float progress() const;
    /// The buffer of the last Finished outcome.
    [[nodiscard]] uint32_t finished_buffer() const { return m_finished_buffer; }

    [[nodiscard]] std::array<TakeBuffer, 2>& buffers() { return m_buffers; }
    [[nodiscard]] const std::array<TakeBuffer, 2>& buffers() const { return m_buffers; }

private:
    struct Take {
        bool m_active = false;
        bool m_bar = false;
        bool m_aligned = false;  // bar take from a bar line (m_bar_aligned_takes)
        bool m_snap = false;     // free take snapped onto 1 to 16 bars (m_bar_aligned_takes)
        bool m_beats = false;
        bool m_saw_touch = false;
        bool m_overwrite = false;  // loops until end() (m_overwrite_loops)
        bool m_full = false;       // overwrite: every point holds motion (seeded, or a pass done)
        uint32_t m_buffer = 0;
        size_t m_capacity = 0;  // bar: points per loop; free: max points
        size_t m_target = 0;    // progress target (snapped free: the next bar boundary)
        size_t m_points_per_bar = 0;
        size_t m_count = 0;            // points written (overwrite: into this pass)
        size_t m_count_at_change = 0;  // points up to the last input change
        size_t m_start_index = 0;      // bar: physical index of the first point
        double m_clock = 0.0;          // samples (Seconds) or beats since start
        double m_tick_base = 0.0;      // clock of tick 0
        double m_tick_step = 0.0;      // clock units per point
        double m_start_beat = 0.0;
        double m_length = 0.0;     // bar: loop length in beats
        double m_bar_beats = 4.0;  // beats per bar at the start
        double m_bar_line = 0.0;   // aligned: beat of point 0 (overwrite: the loop line)
        float m_last_x = 0.0f;
        float m_last_y = 0.0f;
    };

    void write_point(float x, float y, uint8_t gate) TANH_NONBLOCKING_FUNCTION;
    void seed(const TakeSeed& seed) TANH_NONBLOCKING_FUNCTION;
    void trim_still_ends() TANH_NONBLOCKING_FUNCTION;
    void close_seam(TakeBuffer& buffer, size_t num_points) TANH_NONBLOCKING_FUNCTION;
    [[nodiscard]] Outcome finish(double bpm, uint32_t trim_samples = 0) TANH_NONBLOCKING_FUNCTION;
    [[nodiscard]] Outcome drop(Outcome outcome) TANH_NONBLOCKING_FUNCTION;

    std::atomic<uint32_t>& m_next_take_id;
    MotionRecorderConfig m_config;
    double m_sample_rate = 48000.0;
    LoopEnd m_loop_end = LoopEnd::Smooth;
    Take m_take;
    uint32_t m_finished_buffer = 0;
    std::array<TakeBuffer, 2> m_buffers;
};

}  // namespace thl::modulation::detail
