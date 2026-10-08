#pragma once

#include <tanh/core/Exports.h>
#include <tanh/core/threading/LockFreeQueue.h>
#include <tanh/core/threading/RCU.h>
#include <tanh/dsp/transport/TransportInfo.h>
#include <tanh/modulation/MotionLane.h>
#include <tanh/modulation/MotionLaneProcessing.h>
#include <tanh/modulation/MotionShape.h>
#include <tanh/modulation/MotionTypes.h>
#include <tanh/modulation/detail/MotionGlide.h>
#include <tanh/modulation/detail/MotionHistory.h>
#include <tanh/modulation/detail/MotionTakes.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace thl::modulation {

namespace detail {

enum class MotionCommandKind : uint8_t { Arm, Record, Disarm, Play, Stop, Reverse, PlaybackLength };

/// One UI to audio recorder command.
struct MotionCommand {
    MotionCommandKind m_kind = MotionCommandKind::Play;
    uint8_t m_arg = 0;
};

}  // namespace detail

/// Capacity of a recorder's UI to audio command queue.
inline constexpr size_t k_motion_command_capacity = 32;

/// Beats-lane playback while the transport is stopped.
enum class StoppedTransport : uint8_t {
    FreeRun,  ///< keep moving at the last tempo from the last beat (default)
    Hold      ///< freeze the output
};

/// Recorder state as a UI shows it.
enum class MotionState : uint8_t {
    Idle,       ///< nothing playing (no lane, or playback stopped) and not armed
    Armed,      ///< waiting for the first touch; the old lane keeps playing
    Recording,  ///< a take is being written
    Playing     ///< a lane is playing
};

/// One block of touch input for MotionRecorder::process().
struct MotionInput {
    const float* m_x = nullptr;  ///< [0, 1]
    const float* m_y = nullptr;  ///< [0, 1]
    const uint8_t* m_active = nullptr;
    std::span<const uint32_t> m_change_points;  ///< sorted offsets where an input event landed
    uint32_t m_num_samples = 0;
};

/// Recorder state at the end of the last process() call.
struct MotionSnapshot {
    MotionState m_state = MotionState::Idle;
    bool m_playing = false;  ///< playback enabled (play() / stop())
    bool m_reverse = false;
    bool m_beats = false;     ///< the playing lane is a Beats lane
    bool m_busy = false;      ///< armed, but both take buffers wait for service()
    bool m_refused = false;   ///< the last take could not start (tpb below the minimum)
    bool m_aborted = false;   ///< the last take was aborted (prepare() during a take)
    float m_phase = 0.0f;     ///< playback position, 0..1 of the loop
    float m_progress = 0.0f;  ///< recording progress, 0..1 of the current loop target
    uint32_t m_take_id = 0;   ///< id of the lane that is playing (0 = none)
    double m_length = 0.0;    ///< loop length of that lane (seconds or beats)
};

/**
 * @brief Records an XY gesture and loops it back, synced to the transport.
 *
 * Arm, touch, and the gesture is recorded on the audio thread into a
 * preallocated take buffer. A take is either free (ends on release) or 1 to 16
 * bars (ends after exactly one loop). Playback starts on the next sample and
 * follows the transport tempo; a live touch always overrides it, and the output
 * glides back on release. service() publishes finished takes as MotionLane
 * through RCU on the message thread.
 *
 * A new take replaces the playing lane from its first sample: the old lane
 * stops playing, and if the take is then dropped (a cancelled bar take, a take
 * too short to keep) service() clears it.
 *
 * service() publishes the raw take and plays a copy whose seam is closed and
 * which is smoothed by set_smoothing(). Every publication can be undone once
 * (undo(), redo()).
 *
 * set_shapes() plays stock shapes in place of the lane, or morphs between a
 * shape and the lane (MotionShapeMix). A shape loops over the playback length
 * (one bar while it is Free), or over the lane's loop when a slot plays the
 * take. While a shape plays, a touch moves its centre instead of replacing it;
 * shapes pause while a take records.
 *
 * XYController owns one recorder per voice and calls process(); recorder(v)
 * gives the UI and message thread access to the rest.
 *
 * Threading:
 * - ctor, prepare: message thread, audio stopped (allocates).
 * - arm, record, disarm, play, stop, set_reverse, set_playback_length:
 *   one UI thread (lock-free queue).
 * - set_stopped_transport, smoothing, loop_end, set_shapes, shapes,
 *   shapes_version: any thread.
 * - process and the audio getters: audio thread.
 * - service, load_lane, clear, undo, redo, can_undo, can_redo, clear_history,
 *   set_smoothing, set_loop_end: one message thread (RCU publication, allocates).
 * - lane, played_lane, played_path, read_lane, lane_version: message or UI thread.
 */
class TANH_API MotionRecorder {
public:
    MotionRecorder();
    ~MotionRecorder();

    MotionRecorder(const MotionRecorder&) = delete;
    MotionRecorder& operator=(const MotionRecorder&) = delete;
    MotionRecorder(MotionRecorder&&) = delete;
    MotionRecorder& operator=(MotionRecorder&&) = delete;

    /**
     * @brief Allocate everything for blocks of up to @p max_block_size samples.
     *
     * Publishes finished takes first and aborts a take that is still recording
     * (the previous lane is kept). Playback re-locks to the next block's beat
     * without a glide.
     */
    void prepare(double sample_rate,
                 size_t max_block_size,
                 const MotionRecorderConfig& config = MotionRecorderConfig{});

    [[nodiscard]] const MotionRecorderConfig& config() const { return m_config; }
    /// Bytes allocated for the two take buffers.
    [[nodiscard]] size_t take_buffer_bytes() const;

    /// Arm a take; it starts on the first touch, or at once if a touch is held.
    /// UI-thread commands return false when the command queue
    /// (k_motion_command_capacity) is full; the command is then dropped.
    [[nodiscard]] bool arm(LoopLength length);
    /// Start a take at the next block without waiting for a touch (a free take
    /// with m_bar_aligned_takes still starts its points at the first touch).
    [[nodiscard]] bool record(LoopLength length);
    /// Cancel arming. A running free take ends there and plays; a bar-aligned
    /// BarsN take is padded to its loop. A take that was never touched, and a
    /// running bar take without m_bar_aligned_takes, is dropped.
    [[nodiscard]] bool disarm();
    /// Enable playback (the default).
    [[nodiscard]] bool play();
    /// Disable playback (gate 0, x and y hold). Ends a running take like disarm().
    [[nodiscard]] bool stop();
    /// Mirror the playback phase (Beats lanes glide to phase = length - phase).
    [[nodiscard]] bool set_reverse(bool reverse);
    /// Play every lane as @p length: BarsN stretches the lane to exactly N bars of
    /// the current time signature, starting on a bar line (faster or slower than
    /// recorded); Free (the default) plays it at its recorded length.
    [[nodiscard]] bool set_playback_length(LoopLength length);
    /// Behaviour of Beats lanes while the transport is stopped.
    void set_stopped_transport(StoppedTransport mode) {
        m_stopped_mode.store(mode, std::memory_order_relaxed);
    }

    /**
     * @brief Advance one block of at most the prepared size.
     *
     * The transport's discontinuity flags are the only jump detector: k_jumped
     * and k_started re-seek playback (with a glide if the phase moved),
     * k_timeline_reset re-locks without a glide, and otherwise the beat is taken
     * as continuous. A jump never aborts or shifts a running take.
     */
    void process(const thl::dsp::transport::TransportInfo& transport,
                 const MotionInput& input,
                 uint32_t num_samples) TANH_NONBLOCKING_FUNCTION;

    /// Composed x of the last block: live touch over playback, with glides.
    [[nodiscard]] const float* out_x() const TANH_NONBLOCKING_FUNCTION { return m_out_x.data(); }
    [[nodiscard]] const float* out_y() const TANH_NONBLOCKING_FUNCTION { return m_out_y.data(); }
    /// 1 while touched or while the playing lane's gate is open.
    [[nodiscard]] const uint8_t* out_gate() const TANH_NONBLOCKING_FUNCTION {
        return m_out_gate.data();
    }
    /// 1 where a live touch, not playback, produced the sample.
    [[nodiscard]] const uint8_t* out_live() const TANH_NONBLOCKING_FUNCTION {
        return m_out_live.data();
    }
    /// Sorted, unique offsets where the output changed. Shared by x, y and gate.
    [[nodiscard]] std::span<const uint32_t> change_points() const TANH_NONBLOCKING_FUNCTION {
        return {m_change_points.data(), m_num_change_points};
    }
    [[nodiscard]] uint32_t num_samples() const TANH_NONBLOCKING_FUNCTION { return m_num_samples; }

    /// State at the end of the last block (audio thread; the UI reads XYFrame).
    [[nodiscard]] MotionSnapshot snapshot() const TANH_NONBLOCKING_FUNCTION;
    /// Playback phase at the end of the last block, in seconds or beats into the loop.
    [[nodiscard]] double playback_phase() const TANH_NONBLOCKING_FUNCTION { return m_phase; }
    /// Glides started by a flagged transport jump since prepare().
    [[nodiscard]] uint64_t jump_glide_count() const TANH_NONBLOCKING_FUNCTION {
        return m_jump_glides;
    }

    /// Publish finished takes; call from a message-thread timer at 10 Hz or more.
    /// Returns true if a new lane was published.
    bool service();

    /// Replace the lane (preset load). Clamps the points, resamples down to the
    /// capacity and returns the new take id, or 0 if @p lane is malformed.
    uint32_t load_lane(MotionLane lane);

    /// Publish an empty lane: playback stops (gate 0, x and y hold). Returns the new id.
    uint32_t clear();

    /// Republish the lane before the last publication (take, load, clear)
    /// under a new take id. Publishes finished takes first. False if there is none.
    bool undo();
    /// Republish the lane the last undo() replaced. A new publication drops it.
    bool redo();
    [[nodiscard]] bool can_undo() const { return m_history.can_undo(); }
    [[nodiscard]] bool can_redo() const { return m_history.can_redo(); }
    /// Forget the undo and redo lanes (preset load).
    void clear_history();

    /**
     * @brief Playback smoothing, 0 to 1; the published lane stays raw.
     *
     * The played copy spreads the jump at the loop seam over a window that grows
     * from 150 ms (0.3 beat) at 0 to the whole loop at 1, then smooths x and y
     * with a zero-phase Gaussian of sigma = amount^2 * half a beat (250 ms for
     * Seconds lanes). The gate is not smoothed. Playback glides to the result.
     */
    void set_smoothing(float amount);
    [[nodiscard]] float smoothing() const { return m_smoothing.load(std::memory_order_relaxed); }

    /// Loop end of the played lane (LoopEnd::Smooth by default). Jump also
    /// leaves new takes unblended while they play from the take buffer.
    void set_loop_end(LoopEnd mode);
    [[nodiscard]] LoopEnd loop_end() const { return m_loop_end.load(std::memory_order_relaxed); }

    /**
     * @brief Play shapes in place of, or blended with, the lane.
     *
     * Applied from the next block. A slot change glides like a new lane; morph,
     * size and rotation follow at the render interval. With both slots on Take
     * the lane plays as without shapes.
     */
    void set_shapes(const MotionShapeMix& mix);
    [[nodiscard]] MotionShapeMix shapes() const;
    /// Bumped by every set_shapes() that changes the mix: redraw played_path().
    [[nodiscard]] uint32_t shapes_version() const {
        return m_shapes_version.load(std::memory_order_acquire);
    }

    /// Copy of the published lane (raw, with the recorded seam; allocates).
    [[nodiscard]] MotionLane lane() const;
    /// Copy of the published lane with the x and y that play: seam closed and
    /// smoothed (allocates).
    [[nodiscard]] MotionLane played_lane() const;

    /// What plays over one loop: played_lane(), or with shapes (set_shapes()) the
    /// mix sampled at @p num_points points, gate 1 where a shape sounds (allocates).
    [[nodiscard]] MotionLane played_path(size_t num_points = 512) const;

    /// Call @p f with the published (raw) lane inside an RCU read section.
    template <typename F>
    void read_lane(F&& f) const {
        m_lanes.read([&](const PlayedLane& l) { f(l.m_lane); });
    }

    /// Bumped on every publication (take, load, clear, undo, redo,
    /// smoothing or loop end change): redraw when it changes.
    [[nodiscard]] uint32_t lane_version() const {
        return m_lane_version.load(std::memory_order_acquire);
    }

private:
    using CommandKind = detail::MotionCommandKind;
    using Command = detail::MotionCommand;

    using TakeBuffer = detail::TakeBuffer;
    using BufferState = detail::TakeBufferState;
    using TakeOutcome = detail::MotionTakeWriter::Outcome;

    // The RCU payload: the published lane and the x and y that play.
    struct PlayedLane {
        MotionLane m_lane;
        std::vector<float> m_x;
        std::vector<float> m_y;
        bool m_smoothed = false;  // may differ from the finalised take buffer: glide on handoff
        uint32_t m_version = 0;   // per publication, also for a smoothing change
    };

    // What playback reads: a take buffer or the RCU lane, valid for one block.
    struct LaneView {
        const float* m_x = nullptr;  // played (smoothed) x and y
        const float* m_y = nullptr;
        const uint8_t* m_gate = nullptr;
        size_t m_num_points = 0;
        MotionTimebase m_timebase = MotionTimebase::Seconds;
        double m_length = 0.0;
        double m_anchor = 0.0;
        // Shapes play (m_shapes): m_x / m_y / m_gate are the take, or null without one.
        bool m_shaped = false;
        [[nodiscard]] bool empty() const { return m_num_points == 0 || !(m_length > 0.0); }
    };

    enum class Source : uint8_t { Buffer0, Buffer1, Lane };

    void apply_commands(double bpm);
    void select_source(const PlayedLane& lane);
    [[nodiscard]] LaneView view_of(Source source, const PlayedLane& lane) const;
    // view_of() the playing source, empty while a new take replaces it.
    [[nodiscard]] LaneView playback_view(const PlayedLane& lane) const;
    // @p lane with the block's shapes: the lane as the take slot, or a shape loop.
    [[nodiscard]] LaneView shaped_view(LaneView lane) const;
    // Read set_shapes() for the block; a slot change re-seeks with a glide.
    void apply_shapes();
    void switch_to(Source source, uint32_t id, bool silent);
    void glide_in_place();
    // Opens a take at @p offset of this block (recording clock beat @p clock_beat).
    void start_take(const thl::dsp::transport::TransportInfo& transport,
                    const MotionInput& input,
                    uint32_t offset,
                    double clock_beat);
    // What a take that ended or stopped means for arming and playback.
    void on_take(TakeOutcome outcome);
    void start_glide() { m_glide.start(m_last_x, m_last_y); }
    void publish(MotionLane lane, bool record_history = true);
    void publish_played(MotionLane lane);

    MotionRecorderConfig m_config;
    double m_sample_rate = 48000.0;
    size_t m_max_block = 0;

    // Shared between threads.
    thl::core::LockFreeQueue<Command, k_motion_command_capacity> m_commands;
    RCU<PlayedLane> m_lanes;
    RCU<PlayedLane>::Reader m_audio_reader;
    std::atomic<uint32_t> m_next_take_id{1};
    // The running take and the two take buffers (audio thread; service() reads Finished ones).
    detail::MotionTakeWriter m_takes{m_next_take_id};
    std::atomic<uint32_t> m_lane_version{0};
    std::atomic<StoppedTransport> m_stopped_mode{StoppedTransport::FreeRun};
    // Id of a lane a dropped take replaced; service() clears it if still published.
    std::atomic<uint32_t> m_clear_request{0};
    std::atomic<float> m_smoothing{0.0f};
    std::atomic<LoopEnd> m_loop_end{LoopEnd::Smooth};
    AtomicMotionShapeMix m_shape_mix;
    std::atomic<uint32_t> m_shapes_version{0};

    // Message thread only.
    uint32_t m_published_id = 0;
    uint32_t m_played_version = 0;
    detail::MotionHistory m_history;

    // Audio thread only: commands and arming.
    bool m_armed = false;
    bool m_force_start = false;
    LoopLength m_arm_length = LoopLength::Free;
    bool m_play_enabled = true;
    bool m_reverse = false;
    bool m_reverse_pending = false;
    LoopLength m_playback_length = LoopLength::Free;
    MotionShapeMix m_shapes;          // set_shapes() for this block
    bool m_shape_gate = false;        // a shape slot sounds: the gate stays open
    uint32_t m_replaced_take_id = 0;  // lane muted by the running take (0 = none)
    int m_sig_num = 4;                // time signature of the last block
    int m_sig_denom = 4;
    bool m_busy = false;
    bool m_refused = false;
    bool m_aborted = false;

    // Audio thread only: playback source and clock.
    Source m_source = Source::Lane;
    uint32_t m_playing_take_id = 0;
    uint32_t m_playing_version = 0;  // PlayedLane::m_version of the playing lane
    bool m_have_clock = false;
    double m_clock_end = 0.0;  // beat at the end of the previous block

    // Playback phase (seconds or beats into the loop) at the start of the
    // current segment, and at the end of the last block.
    double m_segment_phase = 0.0;
    uint32_t m_segment_start = 0;
    double m_phase = 0.0;
    bool m_need_phase = true;  // the next sample re-derives the phase (new lane)
    MotionTimebase m_view_timebase = MotionTimebase::Seconds;
    double m_view_length = 0.0;  // 0: no lane playing

    // Render ramp between Catmull-Rom ticks.
    bool m_ramp_restart = true;
    uint32_t m_ramp_position = 0;
    double m_last_render_phase = 0.0;  // wrap detection
    double m_previous_phase_step = 1.0;
    float m_ramp_from_x = 0.0f;
    float m_ramp_from_y = 0.0f;
    float m_ramp_to_x = 0.0f;
    float m_ramp_to_y = 0.0f;

    detail::MotionGlide m_glide;
    uint64_t m_jump_glides = 0;

    // Output and last state.
    float m_last_x = 0.0f;
    float m_last_y = 0.0f;
    uint8_t m_last_gate = 0;
    bool m_last_live = false;
    uint32_t m_num_samples = 0;
    std::vector<float> m_out_x;
    std::vector<float> m_out_y;
    std::vector<uint8_t> m_out_gate;
    std::vector<uint8_t> m_out_live;
    std::vector<uint32_t> m_change_points;
    size_t m_num_change_points = 0;
};

}  // namespace thl::modulation
