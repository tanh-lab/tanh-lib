#pragma once

#include <tanh/core/AbiNamespace.h>
#include <tanh/core/Exports.h>
#include <tanh/core/threading/LockFreeQueue.h>
#include <tanh/dsp/transport/TransportInfo.h>
#include <tanh/modulation/MotionLane.h>
#include <tanh/modulation/MotionLaneProcessing.h>
#include <tanh/modulation/MotionTransform.h>
#include <tanh/modulation/MotionTypes.h>
#include <tanh/modulation/detail/MotionClock.h>
#include <tanh/modulation/detail/MotionCompositor.h>
#include <tanh/modulation/detail/MotionLanePlayer.h>
#include <tanh/modulation/detail/MotionLaneStore.h>
#include <tanh/modulation/detail/MotionTakeRecorder.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace thl::inline THL_ABI_NAMESPACE::modulation {

namespace detail {

enum class MotionCommandKind : uint8_t { Arm, Record, Disarm, Play, Stop, Reverse, PlaybackLength };

/// One UI to audio recorder command.
struct MotionCommand {
    MotionCommandKind m_kind = MotionCommandKind::Play;
    uint8_t m_arg = 0;
    LoopLength m_length;  ///< Arm, Record, PlaybackLength
};

/// TakeOptions in a command's m_arg (Arm, Record).
[[nodiscard]] constexpr uint8_t pack_take_options(TakeOptions options) {
    return static_cast<uint8_t>((options.m_free == FreeTake::UntilStopped ? 1U : 0U) |
                                (options.m_bars == BarsTake::Replace ? 2U : 0U));
}
[[nodiscard]] constexpr TakeOptions unpack_take_options(uint8_t arg) {
    return {.m_free = (arg & 1U) != 0 ? FreeTake::UntilStopped : FreeTake::Gesture,
            .m_bars = (arg & 2U) != 0 ? BarsTake::Replace : BarsTake::Overdub};
}

}  // namespace detail

/// Capacity of a recorder's UI to audio command queue.
inline constexpr size_t k_motion_command_capacity = 32;

/// Beats-lane playback while the transport is stopped.
enum class StoppedTransport : uint8_t {
    FreeRun,  ///< keep moving at the last tempo from the last beat (default)
    Hold      ///< freeze the output
};

/**
 * @brief Records a gesture of one or more channels and loops it back, synced to the transport.
 *
 * A channel is one value in [0, 1]; a recorder has a fixed number of them, given to its
 * constructor: one for a slider, two (x, y) for an XY pad.
 *
 * Arm, touch, and the gesture is recorded on the audio thread into a preallocated take
 * (detail::MotionTakeRecorder). Every take is a Beats loop of bars:
 *
 * - Free: one gesture, from the touch to the release, stretched onto the nearest of 1, 2, 4,
 *   8 or 16 bars. With FreeTake::UntilStopped it records on through the lifts (gate closed)
 *   until disarm() or stop(): a drawing of several strokes, with its pauses.
 * - bars(n, division): a looper over n / division bars that records until disarm() or stop().
 *   It starts as what plays; the finger overwrites where it is down and the loop plays on where
 *   it is up. Every release hands the loop over as it is now, so it plays smoothed and
 *   transformed between touches; the whole take is one undo step. With BarsTake::Replace every
 *   touch starts the loop over: what it draws, then its last position held for the rest.
 *
 * A take hands its loop over (detail::MotionLaneStore) and it plays from the next block;
 * service() publishes it as a MotionLane through RCU on the message thread. Until then, and
 * for a take that keeps nothing, the lane that played plays on. Playback
 * (detail::MotionLanePlayer) follows the transport and plays the lane smoothed
 * (set_smoothing()) and moved by the application's transform (set_transform()); a live touch
 * always overrides it, and the output glides back on release (detail::MotionCompositor).
 *
 * Its owner (a control: one recorder per slider or per XY dot, fed by its input) calls process()
 * on the audio thread and gives the UI and message thread access to the rest.
 *
 * Threading:
 * - ctor, prepare, set_transform: message thread, audio stopped (prepare allocates).
 *   load_lane works before the first prepare (a host restores state before it prepares).
 * - arm, record, disarm, play, stop, set_reverse, set_playback_length: one UI thread
 *   (lock-free queue).
 * - set_stopped_transport, smoothing, loop_end, set_transform_settings, transform_settings,
 *   transform_version: any thread.
 * - process and the audio getters: audio thread.
 * - service, load_lane, clear, undo, redo, can_undo, can_redo, clear_history, set_smoothing,
 *   set_loop_end: one message thread (RCU publication, allocates).
 * - lane, played_lane, played_path, read_lane, lane_version: message or UI thread.
 */
class TANH_API MotionRecorder {
public:
    /// A recorder of @p num_channels channels, clamped to [1, k_motion_max_channels].
    explicit MotionRecorder(uint32_t num_channels = 1)
        : m_store(clamp_channels(num_channels)), m_num_channels(clamp_channels(num_channels)) {}
    ~MotionRecorder() = default;

    MotionRecorder(const MotionRecorder&) = delete;
    MotionRecorder& operator=(const MotionRecorder&) = delete;
    MotionRecorder(MotionRecorder&&) = delete;
    MotionRecorder& operator=(MotionRecorder&&) = delete;

    /**
     * @brief Allocate everything for blocks of up to @p samples_per_block samples.
     *
     * Publishes handed-over takes first and aborts a take that is still recording (the lane
     * that played is kept). Playback re-locks to the next block's beat without a glide.
     */
    void prepare(double sample_rate,
                 size_t samples_per_block,
                 const MotionRecorderConfig& config = MotionRecorderConfig{});

    [[nodiscard]] const MotionRecorderConfig& config() const { return m_config; }
    /// Bytes allocated for the take and the two hand-over slots.
    [[nodiscard]] size_t take_buffer_bytes() const;

    /// Arm a take; it starts on the first touch, or at once if a touch is held. @p options: when
    /// a free take ends and what a touch does to a bars take.
    /// UI-thread commands return false when the command queue (k_motion_command_capacity) is
    /// full; the command is then dropped.
    [[nodiscard]] bool arm(LoopLength length, TakeOptions options = {});
    /// Start a take at the next block without waiting for a touch (a free take still starts
    /// its points at the first touch).
    [[nodiscard]] bool record(LoopLength length, TakeOptions options = {});
    /// Cancel arming. A running take ends there and plays (a take never touched keeps nothing).
    [[nodiscard]] bool disarm();
    /// Enable playback (the default).
    [[nodiscard]] bool play();
    /// Disable playback (gate 0, the values hold). Ends a running take like disarm().
    [[nodiscard]] bool stop();
    /// Mirror the playback phase (Beats lanes glide to phase = length - phase).
    [[nodiscard]] bool set_reverse(bool reverse);
    /// Play every lane as @p length: bars(n, division) stretches the lane onto exactly
    /// n / division bars of the current time signature from a bar line; free() (the default)
    /// plays its recorded length.
    [[nodiscard]] bool set_playback_length(LoopLength length);
    /// Behaviour of Beats lanes while the transport is stopped.
    void set_stopped_transport(StoppedTransport mode) {
        m_stopped_mode.store(mode, std::memory_order_relaxed);
    }

    /**
     * @brief Advance one block of at most the prepared size.
     *
     * The transport's discontinuity flags are the only jump detector: k_jumped and k_started
     * re-seek playback (with a glide if the phase moved), k_timeline_reset re-locks without a
     * glide, and otherwise the beat is taken as continuous. A jump never aborts or shifts a
     * running take.
     */
    void process(const thl::dsp::transport::TransportInfo& transport,
                 const MotionInput& input,
                 uint32_t num_samples) TANH_NONBLOCKING_FUNCTION;

    /// Channels of every input, output and lane (the constructor's, clamped).
    [[nodiscard]] uint32_t num_channels() const { return m_num_channels; }
    /// Composed values of @p channel (< num_channels()) in the last block: live touch over
    /// playback, with glides.
    [[nodiscard]] const float* out(uint32_t channel) const TANH_NONBLOCKING_FUNCTION {
        assert(channel < m_num_channels);
        return m_output.values(channel);
    }
    /// 1 while touched or while the playing lane's gate is open.
    [[nodiscard]] const uint8_t* out_gate() const TANH_NONBLOCKING_FUNCTION {
        return m_output.gate();
    }
    /// 1 where a live touch, not playback, produced the sample.
    [[nodiscard]] const uint8_t* out_live() const TANH_NONBLOCKING_FUNCTION {
        return m_output.live();
    }
    /// Sorted, unique offsets where the output changed. Shared by every channel and the gate.
    [[nodiscard]] std::span<const uint32_t> change_points() const TANH_NONBLOCKING_FUNCTION {
        return m_output.change_points();
    }
    [[nodiscard]] uint32_t num_samples() const TANH_NONBLOCKING_FUNCTION { return m_num_samples; }

    /// State at the end of the last block (audio thread; the owner publishes it to the UI).
    [[nodiscard]] MotionSnapshot snapshot() const TANH_NONBLOCKING_FUNCTION;
    /// Playback phase at the end of the last block, in seconds or beats into the loop.
    [[nodiscard]] double playback_phase() const TANH_NONBLOCKING_FUNCTION {
        return m_player.phase();
    }
    /// Glides started by a flagged transport jump since prepare().
    [[nodiscard]] uint64_t jump_glide_count() const TANH_NONBLOCKING_FUNCTION {
        return m_player.jump_glides();
    }

    /// Publish handed-over takes; call from a message-thread timer at 10 Hz or more.
    /// Returns true if a new lane was published.
    bool service();

    /// Replace the lane (preset load). Clamps the points, resamples down to the capacity and
    /// returns the new take id, or 0 if @p lane is malformed or has points and another number
    /// of channels. Also before prepare().
    uint32_t load_lane(MotionLane lane) {
        return m_store.load(std::move(lane), m_config.m_max_points);
    }
    /// Publish an empty lane: playback stops (gate 0, the values hold). A running take ends with
    /// it, keeping nothing, and arming is cancelled. Returns the new id.
    uint32_t clear();

    /// Republish the lane before the last publication (a take, a load, a clear) under a new
    /// take id. Publishes handed-over takes first. False if there is none.
    bool undo() { return m_store.undo(); }
    /// Republish the lane the last undo() replaced. A new publication drops it.
    bool redo() { return m_store.redo(); }
    [[nodiscard]] bool can_undo() const { return m_store.can_undo(); }
    [[nodiscard]] bool can_redo() const { return m_store.can_redo(); }
    /// Forget the undo and redo lanes (preset load).
    void clear_history() { m_store.clear_history(); }

    /**
     * @brief Playback smoothing, 0 to 1; the published lane stays raw.
     *
     * What plays spreads the jump at the loop seam over a window that grows from 150 ms
     * (0.3 beat) at 0 to the whole loop at 1, then smooths every channel with a zero-phase Gaussian
     * of sigma = amount^2 * half a beat (250 ms for Seconds lanes). The gate is not smoothed.
     * Playback glides to the result.
     */
    void set_smoothing(float amount) { m_store.set_smoothing(amount); }
    [[nodiscard]] float smoothing() const { return m_store.smoothing(); }

    /// Loop end of the played lane (LoopEnd::Smooth by default). Jump also leaves a handed-over
    /// take open until it is published.
    void set_loop_end(LoopEnd mode) { m_store.set_loop_end(mode); }
    [[nodiscard]] LoopEnd loop_end() const { return m_store.loop_end(); }

    /// What moves playback point by point (shapes, scaling); null: the lane as it is. The
    /// transform must outlive the recorder.
    void set_transform(const MotionTransform* transform) { m_transform = transform; }
    [[nodiscard]] const MotionTransform* transform() const { return m_transform; }
    /// The transform's setting, applied from the next block; a change bumps
    /// transform_version().
    void set_transform_settings(uint64_t settings);
    [[nodiscard]] uint64_t transform_settings() const {
        return m_transform_settings.load(std::memory_order_acquire);
    }
    /// Bumped by every set_transform_settings() that changes the setting: redraw played_path().
    [[nodiscard]] uint32_t transform_version() const {
        return m_transform_version.load(std::memory_order_acquire);
    }

    /// Copy of the published lane (raw, with the recorded seam; allocates).
    [[nodiscard]] MotionLane lane() const { return m_store.lane(); }
    /// Copy of the published lane with the channels that play: seam closed and smoothed
    /// (allocates).
    [[nodiscard]] MotionLane played_lane() const { return m_store.played_lane(); }
    /// What plays over one loop: played_lane() moved by the transform, sampled at
    /// @p num_points points (allocates).
    [[nodiscard]] MotionLane played_path(size_t num_points = 512) const;

    /// Call @p f with the published (raw) lane inside an RCU read section.
    template <typename F>
    void read_lane(F&& f) const {
        m_store.read_lane(std::forward<F>(f));
    }

    /// Bumped on every publication (take, load, clear, undo, redo, smoothing or loop end
    /// change): redraw when it changes.
    [[nodiscard]] uint32_t lane_version() const { return m_store.version(); }

private:
    using CommandKind = detail::MotionCommandKind;
    using Command = detail::MotionCommand;
    using TakeEvent = detail::MotionTakeRecorder::Event;

    [[nodiscard]] static uint32_t clamp_channels(uint32_t n) {
        return std::clamp<uint32_t>(n, 1, static_cast<uint32_t>(k_motion_max_channels));
    }
    void apply_commands() TANH_NONBLOCKING_FUNCTION;
    void start_take(const dsp::transport::TransportInfo& transport,
                    double beat,
                    const MotionValues& values,
                    const detail::MotionLaneSource& playing) TANH_NONBLOCKING_FUNCTION;
    void on_take(TakeEvent event) TANH_NONBLOCKING_FUNCTION;
    void submit_finished() TANH_NONBLOCKING_FUNCTION;

    // Ordered by alignment and size (clang-analyzer padding check), not by thread.
    // Threads: m_commands, the atomics and m_store are shared; every other field belongs to
    // the audio thread (m_config and m_transform are set while it is stopped).
    thl::core::LockFreeQueue<Command, k_motion_command_capacity> m_commands;
    MotionRecorderConfig m_config;
    detail::MotionLaneStore m_store;
    detail::MotionTakeRecorder m_takes;
    detail::MotionLanePlayer m_player;
    detail::MotionCompositor m_output;
    detail::MotionClock m_clock;
    const MotionTransform* m_transform = nullptr;
    std::atomic<uint64_t> m_transform_settings{0};
    std::atomic<uint32_t> m_transform_version{0};
    std::atomic<StoppedTransport> m_stopped_mode{StoppedTransport::FreeRun};
    double m_sample_rate = 48000.0;
    uint32_t m_num_samples = 0;
    uint32_t m_num_channels = 1;             // set at construction
    std::atomic<uint32_t> m_take_serial{0};  // written by the audio thread, read by clear()
    std::atomic<bool> m_clear_takes{false};  // clear(): end the take at the next block
    // A take that kept nothing: the id of the lane it replaced, for service() to delete.
    std::atomic<uint32_t> m_clear_request{0};
    uint32_t m_replaced_id = 0;  // the lane a take replaced: it no longer plays
    int m_sig_num = 4;
    int m_sig_denom = 4;
    LoopLength m_arm_length = LoopLength::free();
    TakeOptions m_arm_options;
    LoopLength m_playback_length = LoopLength::free();
    bool m_armed = false;
    bool m_force_start = false;
    bool m_play_enabled = true;
    bool m_reverse = false;
    bool m_finish_pending = false;  // a finished take waits for a free hand-over slot
    bool m_handed_over = false;     // a take was handed over this block: play it from here
    bool m_refused = false;
    bool m_aborted = false;
};

}  // namespace thl::inline THL_ABI_NAMESPACE::modulation
