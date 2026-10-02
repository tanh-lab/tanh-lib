#pragma once

#include <tanh/core/Exports.h>
#include <tanh/core/threading/LockFreeQueue.h>
#include <tanh/core/threading/RCU.h>
#include <tanh/dsp/transport/TransportInfo.h>
#include <tanh/modulation/ModulationSource.h>
#include <tanh/modulation/MotionLane.h>
#include <tanh/modulation/XYPad.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace thl::modulation {

class MotionRecorder;

namespace detail {

enum class MotionCommandKind : uint8_t { Arm, Record, Disarm, Play, Stop, Reverse };

/// One UI → audio recorder command.
struct MotionCommand {
    MotionCommandKind m_kind = MotionCommandKind::Play;
    uint8_t m_arg = 0;
};

}  // namespace detail

/// Loop length of the next take. Free: as long as the gesture; BarsN: N bars of
/// the time signature at arm time, synced to the transport.
enum class LoopLength : uint8_t { Free, Bars1, Bars2, Bars4, Bars8, Bars16 };

/// Beats-lane playback while the transport is stopped.
enum class StoppedTransport : uint8_t {
    FreeRun,  ///< keep moving at the last tempo from the last beat (default)
    Hold      ///< freeze the output
};

/// Recorder state as the UI shows it.
enum class MotionState : uint8_t {
    Idle,       ///< nothing playing (no lane, or playback stopped) and not armed
    Armed,      ///< waiting for the first touch; the old lane keeps playing
    Recording,  ///< a take is being written
    Playing     ///< a lane is playing
};

struct MotionRecorderConfig {
    /// Points per take buffer (two buffers per recorder, 9 bytes per point).
    size_t m_max_points = k_motion_default_max_points;
    /// Grid rate of Seconds takes in points per second. Beats takes use
    /// round(rate * 60 / bpm) ticks per beat, clamped to [m_min_tpb, m_max_tpb].
    double m_rate = 200.0;
    uint32_t m_min_tpb = 24;  ///< below this a bar take is refused (capacity)
    uint32_t m_max_tpb = 512;
    /// Catmull-Rom is evaluated every this many samples; the output ramps linearly between.
    uint32_t m_render_interval = 32;
    double m_glide_ms = 25.0;  ///< raised-cosine glide after a jump, a new lane or a release
};

/**
 * @brief Recorder state for a UI; fields come from packed atomics written once
 *        per block (they may be one block apart from each other).
 */
struct MotionSnapshot {
    MotionState m_state = MotionState::Idle;
    bool m_playing = false;  ///< playback enabled (play() / stop())
    bool m_reverse = false;
    bool m_beats = false;     ///< the playing lane is a Beats lane
    bool m_busy = false;      ///< armed, but both take buffers wait for service()
    bool m_refused = false;   ///< the last take could not start (tpb below the minimum)
    bool m_aborted = false;   ///< the last take was aborted (prepare() during a take)
    float m_phase = 0.0f;     ///< playback position, 0..1 of the loop
    float m_progress = 0.0f;  ///< recording progress, 0..1 (bar takes: of the loop; free: of
                              ///< capacity)
    uint32_t m_take_id = 0;   ///< id of the lane that is playing (0 = none)
    double m_length = 0.0;    ///< loop length of that lane (seconds or beats)
};

/**
 * @brief One of the three optional matrix sources of a MotionRecorder (x, y, gate).
 *
 * Global scope, not fully active: the active mask is the output gate.
 * pre_process_block() copies the recorder's last process() output, so the owner
 * must call MotionRecorder::process() before ModulationMatrix::process().
 */
class TANH_API MotionRecorderOutput final : public ModulationSource {
public:
    MotionRecorderOutput(MotionRecorder& recorder, XYPadAxis axis);
    void prepare(double sample_rate, size_t samples_per_block, uint32_t voice_count) override;
    void pre_process_block() override;

private:
    MotionRecorder& m_recorder;
    const XYPadAxis m_axis;
};

/**
 * @class MotionRecorder
 * @brief Records the gesture of one XY pad and loops it back, synced to the transport.
 *
 * Arm, touch, and the gesture is recorded on the audio thread into a
 * preallocated take buffer at control rate (200 points/s, or a tick-per-beat grid
 * while the transport plays). A take is either free (ends on release) or 1–16
 * bars (ends after exactly one loop). Playback starts on the next sample,
 * interpolates with uniform Catmull-Rom across the loop seam and follows the
 * transport tempo; a live touch always overrides it, and the output glides back
 * on release. Finished takes are published as MotionLane through RCU by
 * service() on the message thread.
 *
 * @par Per block (audio thread)
 * The owner — an XY controller's driver step, or the host's processBlock() — calls
 * @code
 * pad.process_block(n);                      // drain touches (pre-matrix)
 * recorder.process(transport, pad.primary(), n);
 * // read out_x() / out_y() / out_gate() / change_points(), or let the matrix copy
 * // them through source(axis) in its process()
 * @endcode
 * The recorder never depends on the matrix's source order and never sees its
 * own output as input.
 *
 * @par Threading
 * | Call | Thread | Mechanism |
 * |---|---|---|
 * | ctor, prepare | message, audio stopped | allocates take buffers, output, reader slot |
 * | arm, record, disarm, play, stop, set_reverse | one UI thread | lock-free command queue |
 * | set_stopped_transport | any | atomic |
 * | process, out_*, change_points | audio | no allocation, locks or logging |
 * | service, load_lane, clear | one message thread | RCU::replace (allocates) |
 * | lane, read_lane, lane_version | message / UI | RCU read |
 * | ui_snapshot | any | packed atomics |
 *
 * @par Take handoff
 * Two take buffers move through Free → Recording → Finished → Published → Free.
 * The audio thread writes only a Recording buffer, finalises it in place
 * (Finished, release) and plays straight from it. service() copies a Finished
 * buffer into a MotionLane, publishes it and marks it Published; the audio thread
 * then switches to the RCU lane (same take id: bit-identical data, no glide) and
 * frees the buffer. The audio thread never publishes through RCU.
 *
 * Take ids come from one counter shared with load_lane()/clear(), and the audio
 * thread always plays the newest id: a lane loaded after a take finished
 * replaces it, a take finished after a load replaces that.
 */
class TANH_API MotionRecorder {
public:
    MotionRecorder();
    ~MotionRecorder();

    MotionRecorder(const MotionRecorder&) = delete;
    MotionRecorder& operator=(const MotionRecorder&) = delete;
    MotionRecorder(MotionRecorder&&) = delete;
    MotionRecorder& operator=(MotionRecorder&&) = delete;

    // ── Message thread, audio stopped ──────────────────────────────────────

    /**
     * @brief Allocate everything for blocks of up to @p max_block_size samples.
     *
     * Publishes finished takes first. A take that is still recording is aborted
     * (the previous lane is kept; ui_snapshot().m_aborted). Playback re-locks to
     * the next block's beat without a glide.
     */
    void prepare(double sample_rate,
                 size_t max_block_size,
                 const MotionRecorderConfig& config = MotionRecorderConfig{});

    [[nodiscard]] const MotionRecorderConfig& config() const { return m_config; }
    /// Bytes allocated for the two take buffers.
    [[nodiscard]] size_t take_buffer_bytes() const;

    // ── UI thread (single producer): lock-free; false when the queue is full ─

    /// Arm a take; it starts on the first touch (or at once if a touch is held).
    bool arm(LoopLength length);
    /// Start a take at the next block, without waiting for a touch.
    bool record(LoopLength length);
    /// Cancel arming. A running free take finishes (and plays); a running bar take is dropped.
    bool disarm();
    /// Enable playback (the default).
    bool play();
    /// Disable playback (gate 0, x/y hold). Ends a running take like disarm().
    bool stop();
    /// Mirror the playback phase (Beats lanes: phase = length - phase, with a glide).
    bool set_reverse(bool reverse);
    /// Behaviour of Beats lanes while the transport is stopped (any thread).
    void set_stopped_transport(StoppedTransport mode) {
        m_stopped_mode.store(mode, std::memory_order_relaxed);
    }

    // ── Audio thread ───────────────────────────────────────────────────────

    /**
     * @brief Advance one block.
     *
     * @param transport the block's TransportInfo (beat, slope, tempo, play state,
     *                  time signature; discontinuity flags are optional)
     * @param input     the pad's primary stream for this block (XYPad::primary()),
     *                  pre-matrix; a default-constructed stream means "no touch"
     * @param num_samples block length (≤ the prepared maximum)
     */
    void process(const thl::dsp::transport::TransportInfo& transport,
                 const XYPadStream& input,
                 uint32_t num_samples) noexcept TANH_NONBLOCKING_FUNCTION;

    /// Output of the last process(): composed x / y (touch > playback, with glides) …
    [[nodiscard]] const float* out_x() const noexcept TANH_NONBLOCKING_FUNCTION {
        return m_out_x.data();
    }
    [[nodiscard]] const float* out_y() const noexcept TANH_NONBLOCKING_FUNCTION {
        return m_out_y.data();
    }
    /// … gate: 1 while touched or while the playing lane's gate is open …
    [[nodiscard]] const uint8_t* out_gate() const noexcept TANH_NONBLOCKING_FUNCTION {
        return m_out_gate.data();
    }
    /// … and 1 where a live touch (not playback) produced the sample.
    [[nodiscard]] const uint8_t* out_live() const noexcept TANH_NONBLOCKING_FUNCTION {
        return m_out_live.data();
    }
    /// Sorted, unique offsets where the output changed (render ticks, gate and
    /// touch edges, glide starts). Shared by x, y and gate.
    [[nodiscard]] std::span<const uint32_t> change_points() const noexcept
        TANH_NONBLOCKING_FUNCTION {
        return {m_cps.data(), m_num_cps};
    }
    [[nodiscard]] uint32_t num_samples() const noexcept TANH_NONBLOCKING_FUNCTION {
        return m_num_samples;
    }

    /// Audio-thread view of the state (the UI uses ui_snapshot()).
    [[nodiscard]] MotionState state() const noexcept TANH_NONBLOCKING_FUNCTION;
    /// Playback phase at the end of the last block (seconds or beats into the loop).
    [[nodiscard]] double playback_phase() const noexcept TANH_NONBLOCKING_FUNCTION {
        return m_phase;
    }
    /// Take id of the lane that is playing (0 = none).
    [[nodiscard]] uint32_t playing_take_id() const noexcept TANH_NONBLOCKING_FUNCTION {
        return m_play_id;
    }
    /// Glides started by a transport jump since prepare() (diagnostics, tests).
    [[nodiscard]] uint64_t jump_glide_count() const noexcept TANH_NONBLOCKING_FUNCTION {
        return m_jump_glides;
    }

    /// The matrix source for one axis (X, Y, Active = gate); see MotionRecorderOutput.
    [[nodiscard]] ModulationSource& source(XYPadAxis axis);

    // ── Message thread ─────────────────────────────────────────────────────

    /**
     * @brief Publish finished takes (call from a timer, ≥ 10 Hz).
     * @return true if a new lane was published.
     */
    bool service();

    /**
     * @brief Replace the lane (preset load). Validates sizes, resamples down to
     * the capacity, assigns a new take id and returns it (0 if @p lane is
     * malformed). The audio thread glides to it.
     */
    uint32_t load_lane(MotionLane lane);

    /// Publish an empty lane: playback stops (gate 0, x/y hold). Returns the new id.
    uint32_t clear();

    /// Copy of the published lane (allocates).
    [[nodiscard]] MotionLane lane() const;

    /// Call @p f with the published lane inside an RCU read section.
    template <typename F>
    void read_lane(F&& f) const {
        m_lanes.read([&](const MotionLane& l) { f(l); });
    }

    /// Bumped on every publication (take, load, clear): redraw when it changes.
    [[nodiscard]] uint32_t lane_version() const {
        return m_lane_version.load(std::memory_order_acquire);
    }

    // ── Any thread ─────────────────────────────────────────────────────────

    [[nodiscard]] MotionSnapshot ui_snapshot() const;

private:
    friend class MotionRecorderOutput;

    using CommandKind = detail::MotionCommandKind;
    using Command = detail::MotionCommand;

    enum class BufferState : uint8_t { Free, Recording, Finished, Published };

    struct TakeBuffer {
        std::atomic<BufferState> m_state{BufferState::Free};
        std::vector<float> m_x;
        std::vector<float> m_y;
        std::vector<uint8_t> m_gate;
        // Written by the audio thread before Finished (release), read after (acquire).
        size_t m_num_points = 0;
        MotionTimebase m_timebase = MotionTimebase::Seconds;
        double m_rate = 0.0;
        double m_length = 0.0;
        double m_anchor = 0.0;
        uint32_t m_take_id = 0;
    };

    /// What playback reads: a take buffer or the RCU lane (valid for one block).
    struct LaneView {
        const float* m_x = nullptr;
        const float* m_y = nullptr;
        const uint8_t* m_gate = nullptr;
        size_t m_num_points = 0;
        MotionTimebase m_timebase = MotionTimebase::Seconds;
        double m_length = 0.0;
        double m_anchor = 0.0;
        [[nodiscard]] bool empty() const { return m_num_points == 0 || !(m_length > 0.0); }
    };

    enum class Source : uint8_t { None, Buffer0, Buffer1, Lane };

    /// The take being recorded (audio thread only).
    struct Take {
        bool m_active = false;
        bool m_bar = false;
        bool m_beats = false;
        bool m_saw_touch = false;
        uint32_t m_buffer = 0;
        size_t m_capacity = 0;         // bar: points per loop; free: max points
        size_t m_count = 0;            // points written
        size_t m_count_at_change = 0;  // points up to the last input change
        size_t m_start_index = 0;      // bar: physical index of the first point
        double m_clock = 0.0;          // samples (Seconds) or beats since/at start
        double m_tick_base = 0.0;      // clock of tick 0
        double m_tick_step = 0.0;      // clock units per point
        double m_start_beat = 0.0;
        double m_length = 0.0;  // bar: loop length in beats
        float m_last_x = 0.0f;
        float m_last_y = 0.0f;
        std::array<float, 5> m_ring_x{};  // raw last points
        std::array<float, 5> m_ring_y{};
    };

    // Audio-thread helpers
    void apply_commands(double bpm) noexcept TANH_NONBLOCKING_FUNCTION;
    void select_source(const MotionLane& lane) noexcept TANH_NONBLOCKING_FUNCTION;
    [[nodiscard]] LaneView view_of(Source src,
                                   const MotionLane& lane) const noexcept TANH_NONBLOCKING_FUNCTION;
    void switch_to(Source src, uint32_t id, bool silent) noexcept TANH_NONBLOCKING_FUNCTION;
    void start_take(const thl::dsp::transport::TransportInfo& t,
                    const XYPadStream& in,
                    uint32_t offset,
                    double clock_beat) noexcept TANH_NONBLOCKING_FUNCTION;
    void write_point(float x, float y, uint8_t gate) noexcept TANH_NONBLOCKING_FUNCTION;
    void finish_take(double bpm, uint32_t trim_samples = 0) noexcept TANH_NONBLOCKING_FUNCTION;
    void abort_take() noexcept TANH_NONBLOCKING_FUNCTION;
    void finalise(TakeBuffer& b,
                  size_t n,
                  double points_per_second) noexcept TANH_NONBLOCKING_FUNCTION;
    void start_glide() noexcept TANH_NONBLOCKING_FUNCTION;
    void publish_snapshot(const LaneView& view) noexcept TANH_NONBLOCKING_FUNCTION;

    // Message-thread helpers
    void publish(MotionLane lane);

    MotionRecorderConfig m_config;
    double m_sample_rate = 48000.0;
    size_t m_max_block = 0;
    uint32_t m_glide_samples = 1200;

    // ── Shared ────────────────────────────────────────────────────────────
    thl::core::LockFreeQueue<Command, 32> m_commands;
    std::array<TakeBuffer, 2> m_takes;
    RCU<MotionLane> m_lanes;
    thl::detail::RcuReaderNode* m_audio_reader = nullptr;
    std::atomic<uint32_t> m_next_take_id{1};
    std::atomic<uint32_t> m_lane_version{0};
    std::atomic<StoppedTransport> m_stopped_mode{StoppedTransport::FreeRun};
    std::atomic<uint64_t> m_snap_bits{0};
    std::atomic<uint32_t> m_snap_take{0};
    std::atomic<double> m_snap_length{0.0};

    // ── Message thread only ───────────────────────────────────────────────
    uint32_t m_published_id = 0;

    // ── Audio thread only ─────────────────────────────────────────────────
    Take m_take;
    bool m_armed = false;
    bool m_force_start = false;
    LoopLength m_arm_length = LoopLength::Free;
    bool m_play_enabled = true;
    bool m_reverse = false;
    bool m_busy = false;
    bool m_refused = false;
    bool m_aborted = false;

    Source m_src = Source::Lane;
    uint32_t m_play_id = 0;
    bool m_reverse_pending = false;
    bool m_snap_has_lane = false;

    // Clock
    bool m_have_clock = false;
    double m_clock_end = 0.0;  // beat at the end of the previous block

    // Playback phase (seconds or beats into the loop) at the start of the
    // current segment, and at the end of the last block.
    double m_seg_phase = 0.0;
    uint32_t m_seg_start = 0;
    double m_phase = 0.0;
    bool m_need_phase = true;  // the next sample re-derives the phase (new lane)

    // Render ramp
    bool m_ramp_restart = true;
    uint32_t m_ramp_pos = 0;     // samples since the last tick
    double m_last_p = 0.0;       // phase of the last rendered sample (wrap detection)
    double m_prev_dphase = 1.0;  // phase slope of the previous block
    float m_ramp_from_x = 0.0f, m_ramp_from_y = 0.0f;
    float m_ramp_to_x = 0.0f, m_ramp_to_y = 0.0f;

    // Glide
    uint32_t m_glide_left = 0;
    float m_glide_from_x = 0.0f, m_glide_from_y = 0.0f;
    uint64_t m_jump_glides = 0;

    // Output and last state
    float m_last_x = 0.0f;
    float m_last_y = 0.0f;
    uint8_t m_last_gate = 0;
    bool m_last_live = false;
    uint32_t m_num_samples = 0;
    std::vector<float> m_out_x;
    std::vector<float> m_out_y;
    std::vector<uint8_t> m_out_gate;
    std::vector<uint8_t> m_out_live;
    std::vector<uint32_t> m_cps;
    size_t m_num_cps = 0;

    MotionRecorderOutput m_src_x;
    MotionRecorderOutput m_src_y;
    MotionRecorderOutput m_src_gate;
};

}  // namespace thl::modulation
