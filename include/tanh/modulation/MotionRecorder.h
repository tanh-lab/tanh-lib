#pragma once

#include <tanh/core/Exports.h>
#include <tanh/core/threading/LockFreeQueue.h>
#include <tanh/core/threading/RCU.h>
#include <tanh/dsp/transport/TransportInfo.h>
#include <tanh/modulation/MotionLane.h>
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

/// Loop length of a take (arm, record) or of playback (set_playback_length).
/// Free lasts as long as the gesture; BarsN is N bars of the time signature,
/// synced to the transport.
enum class LoopLength : uint8_t { Free, Bars1, Bars2, Bars4, Bars8, Bars16 };

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
    /// Free takes end after 16 bars at most and are stretched onto the nearest
    /// (in ratio) of 1, 2, 4, 8 or 16 bars: a Beats lane whose index 0 sits on
    /// the bar line nearest the take's start. Off: free takes keep their length.
    bool m_snap_to_bars = false;
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
    float m_progress = 0.0f;  ///< recording progress, 0..1 of the loop or of the capacity
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
 * XYController owns one recorder per voice and calls process(); recorder(v)
 * gives the UI and message thread access to the rest.
 *
 * Threading:
 * - ctor, prepare: message thread, audio stopped (allocates).
 * - arm, record, disarm, play, stop, set_reverse, set_playback_length: one UI
 *   thread (lock-free queue).
 * - set_stopped_transport: any thread.
 * - process and the audio getters: audio thread.
 * - service, load_lane, clear: one message thread (RCU publication, allocates).
 * - lane, read_lane, lane_version: message or UI thread.
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
    /// Start a take at the next block without waiting for a touch.
    [[nodiscard]] bool record(LoopLength length);
    /// Cancel arming. A running free take finishes and plays; a running bar take is dropped.
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

private:
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

    // What playback reads: a take buffer or the RCU lane, valid for one block.
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

    enum class Source : uint8_t { Buffer0, Buffer1, Lane };

    // The take being recorded (audio thread only).
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
        double m_clock = 0.0;          // samples (Seconds) or beats since start
        double m_tick_base = 0.0;      // clock of tick 0
        double m_tick_step = 0.0;      // clock units per point
        double m_start_beat = 0.0;
        double m_length = 0.0;     // bar: loop length in beats
        double m_bar_beats = 4.0;  // beats per bar at the start
        float m_last_x = 0.0f;
        float m_last_y = 0.0f;
        std::array<float, 5> m_ring_x{};  // raw last points
        std::array<float, 5> m_ring_y{};
    };

    void apply_commands(double bpm);
    void select_source(const MotionLane& lane);
    [[nodiscard]] LaneView view_of(Source source, const MotionLane& lane) const;
    // view_of() the playing source, empty while a new take replaces it.
    [[nodiscard]] LaneView playback_view(const MotionLane& lane) const;
    void switch_to(Source source, uint32_t id, bool silent);
    void start_take(const thl::dsp::transport::TransportInfo& transport,
                    const MotionInput& input,
                    uint32_t offset,
                    double clock_beat);
    void write_point(float x, float y, uint8_t gate);
    void finish_take(double bpm, uint32_t trim_samples = 0);
    // keep_replaced: the old lane plays on (prepare()); otherwise service() clears it.
    void abort_take(bool keep_replaced = false);
    void finalise(TakeBuffer& buffer, size_t num_points, double points_per_second);
    void start_glide();
    void publish(MotionLane lane);

    MotionRecorderConfig m_config;
    double m_sample_rate = 48000.0;
    size_t m_max_block = 0;
    uint32_t m_glide_samples = 1200;

    // Shared between threads.
    thl::core::LockFreeQueue<Command, k_motion_command_capacity> m_commands;
    std::array<TakeBuffer, 2> m_takes;
    RCU<MotionLane> m_lanes;
    thl::detail::RcuReaderNode* m_audio_reader = nullptr;
    std::atomic<uint32_t> m_next_take_id{1};
    std::atomic<uint32_t> m_lane_version{0};
    std::atomic<StoppedTransport> m_stopped_mode{StoppedTransport::FreeRun};
    // Id of a lane a dropped take replaced; service() clears it if still published.
    std::atomic<uint32_t> m_clear_request{0};

    // Message thread only.
    uint32_t m_published_id = 0;

    // Audio thread only: commands and take.
    Take m_take;
    bool m_armed = false;
    bool m_force_start = false;
    LoopLength m_arm_length = LoopLength::Free;
    bool m_play_enabled = true;
    bool m_reverse = false;
    bool m_reverse_pending = false;
    LoopLength m_playback_length = LoopLength::Free;
    uint32_t m_replaced_take_id = 0;  // lane muted by the running take (0 = none)
    int m_sig_num = 4;                // time signature of the last block
    int m_sig_denom = 4;
    bool m_busy = false;
    bool m_refused = false;
    bool m_aborted = false;

    // Audio thread only: playback source and clock.
    Source m_source = Source::Lane;
    uint32_t m_playing_take_id = 0;
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

    // Glide.
    uint32_t m_glide_left = 0;
    float m_glide_from_x = 0.0f;
    float m_glide_from_y = 0.0f;
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
