#pragma once

#include <tanh/core/Exports.h>
#include <tanh/core/threading/LockFreeQueue.h>
#include <tanh/core/threading/TripleBuffer.h>
#include <tanh/dsp/transport/TransportInfo.h>
#include <tanh/modulation/ModulationMatrix.h>
#include <tanh/modulation/ModulationRouting.h>
#include <tanh/modulation/ModulationSource.h>
#include <tanh/modulation/MotionRecorder.h>
#include <tanh/modulation/XYPad.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace thl::modulation {

class XYController;

/// Voices (dots) per controller: one per effect slot in a Single-mode pad.
inline constexpr uint32_t k_xy_controller_max_voices = 16;
/// Fingers tracked at once per controller (UI-side touch table).
inline constexpr uint32_t k_xy_controller_max_touches = k_xy_pad_max_touches;
/// Capacity of the live recording trail (audio → UI, drop-oldest).
inline constexpr size_t k_xy_trail_capacity = 2048;

/**
 * @brief Which layer produced a voice's output.
 *
 * Priority, highest first: Touch > Motion. A future sequencer layer is appended
 * below Motion (value 3) without renumbering.
 */
enum class XYLayer : uint8_t {
    None,    ///< no layer is active: x / y hold, active = 0 (or 1 while latched)
    Touch,   ///< a live touch
    Motion,  ///< motion playback (MotionRecorder, gate open, not live)
};

struct XYControllerConfig {
    /// Source prefix. One voice: "<id>.x", "<id>.y", "<id>.active". Several
    /// voices: "<id>.<v>.x" etc. (v = 0 … num_voices - 1).
    std::string m_id;
    /// Dots driven by this controller, clamped to [1, k_xy_controller_max_voices].
    /// A touch grabs the nearest enabled dot (see XYController::touch()).
    uint32_t m_num_voices = 1;
    /// Fingers per voice (several fingers on one dot: m_mono_priority decides).
    uint32_t m_max_touches = 10;
    MonoPriority m_mono_priority = MonoPriority::Last;
    /// Per-voice recorder (capacity, glide back after release, …).
    MotionRecorderConfig m_recorder;
    /// UI frames are published at most at this rate, plus at once on a state change.
    double m_frame_rate_hz = 240.0;
    /// Samples between live trail points while a voice records.
    uint32_t m_trail_interval = 32;
};

/// Options of XYController::route().
struct XYRouteOptions {
    /// Default: ReplaceHold for X / Y, Replace for Active.
    std::optional<CombineMode> m_combine;
    uint32_t m_priority = 10;
    /// Priority of ReplaceHold's held write (cosmos default: 0 = yields to any live writer).
    std::optional<uint32_t> m_hold_priority = 0u;
    float m_depth = 1.0f;
    /// Replace range in the target's normalized [0, 1] units.
    std::optional<std::pair<float, float>> m_range_normalized;
    bool m_enabled = true;
    uint32_t m_max_decimation = 0;
};

/// One live recording trail point (audio → UI).
struct XYPathPoint {
    float m_x = 0.0f;
    float m_y = 0.0f;
    float m_progress = 0.0f;  ///< recording progress of the voice, 0..1
    uint8_t m_voice = 0;
    uint8_t m_active = 0;
};

/// One voice in an XYFrame. All fields come from the same audio block.
struct XYVoiceFrame {
    float m_x = 0.0f;  ///< controller output at block end (pre-matrix)
    float m_y = 0.0f;
    uint8_t m_active = 0;           ///< gate output (incl. latch)
    bool m_has_value = false;       ///< touched or played at least once (x / y outputs are live)
    bool m_touched = false;         ///< a live touch drives the voice
    bool m_motion_playing = false;  ///< playback enabled
    bool m_reverse = false;
    bool m_beats = false;  ///< the playing lane follows the transport
    XYLayer m_layer = XYLayer::None;
    MotionState m_motion_state = MotionState::Idle;
    float m_motion_phase = 0.0f;     ///< playback position, 0..1 of the loop
    float m_record_progress = 0.0f;  ///< 0..1 while recording
    uint32_t m_take_id = 0;          ///< lane playing (0 = none)
    uint32_t m_path_version = 0;     ///< MotionRecorder::lane_version(): re-read the path on change
    double m_loop_length = 0.0;      ///< seconds or beats
};

/**
 * @brief Fixed-size UI snapshot of one controller, published once per block
 *        (rate-limited) through a TripleBuffer: every field is from the same block.
 */
struct XYFrame {
    static constexpr uint32_t k_max_voices = k_xy_controller_max_voices;
    uint64_t m_sequence = 0;     ///< publication counter (0 = never published)
    uint64_t m_sample_time = 0;  ///< samples driven since prepare(), at block end
    uint32_t m_num_voices = 0;
    uint32_t m_trail_dropped = 0;  ///< trail points lost to a slow UI (cumulative)
    bool m_latch = false;
    std::array<XYVoiceFrame, k_max_voices> m_voices{};
};
static_assert(std::is_trivially_copyable_v<XYFrame>);

/**
 * @brief One matrix source of an XYController voice (x, y or active).
 *
 * The first output's pre_process_block() in a block runs the controller's
 * driver step; process() only copies that voice's block into the output
 * buffers. Masks: x / y are live once the voice has a value (touched or played
 * once), so a re-enabled routing (mode switch) picks the dot up at once;
 * active is the gate.
 */
class TANH_API XYControllerOutput final : public ModulationSource {
public:
    XYControllerOutput(XYController& controller, uint32_t voice, XYPadAxis axis);

    void prepare(double sample_rate, size_t samples_per_block, uint32_t voice_count) override;
    void clear_per_block() override;
    void pre_process_block() override;
    void process(size_t num_samples, size_t offset) override;

private:
    XYController& m_controller;
    const uint32_t m_voice;
    const XYPadAxis m_axis;
};

/**
 * @class XYController
 * @brief Touch pad, motion recorder and UI snapshot for one or more XY dots,
 *        exposed as matrix sources.
 *
 * Each voice (dot) owns an XYPad and a MotionRecorder. Per block the driver step
 * runs, for every voice,
 * @code
 * pad.process_block(n);
 * recorder.process(transport, pad.primary(), n);   // touch > motion, glides
 * // [extension point] a sequencer layer would advance here and fill the
 * // samples no higher layer claims (XYLayer::Sequencer, appended)
 * @endcode
 * and writes x / y / active / layer for the whole block into the voice's
 * buffers; the output sources only copy them in process(). The recorder already
 * composes touch over playback and glides back after a release, so the
 * controller adds no second glide. The driver runs once per block, from
 * whichever output the matrix reaches first: no dependency on the matrix's
 * source order, and controllers share no state.
 *
 * @par Transport and block length
 * pre_process_block() has no length, so the engine hands the block's
 * TransportInfo (with m_num_samples) to every controller before
 * matrix.process(): set_transport(). Without a matrix, process_block(t) runs
 * the driver directly (then the outputs reuse that render in the same block).
 * A transport longer than the prepared block size runs as consecutive chunks of
 * at most that size, like the same audio in prepared-size blocks; the outputs
 * then hold the last chunk. The matrix itself takes no larger block, so the
 * engine splits host blocks (docs: transport.md, "Block size").
 *
 * @par Voices and touches
 * One voice: every touch drives it (several fingers: MonoPriority). Several
 * voices (a Single-mode pad, voice n = slot n): a new touch grabs the nearest
 * enabled voice that no other finger holds (else the nearest enabled one) and
 * moves only that voice until released. Selection runs in touch() on the UI
 * thread against the dot positions of the last block. touch_voice() bypasses
 * selection (keyboard nudges, accessibility).
 *
 * @par Threading
 * | Call | Thread | Notes |
 * |---|---|---|
 * | ctor, dtor, route*, unroute*, set_route_*, service | message | ctor/dtor/route add or remove
 * sources/routings (schedule rebuild, dtor blocks on RCU sync); set_route_enabled is lock-free for
 * audio | | touch, touch_voice, release, release_all, flush | one UI thread | lock-free,
 * allocation-free | | read_frame, drain_trail, read_path | one UI thread | wait-free / lock-free;
 * read_path is an RCU read | | set_latch, set_voice_enabled, set_ui_attached | any | atomics | |
 * recorder(v) commands / load_lane / clear | as MotionRecorder | | | set_transport, process_block,
 * reset, out_* | audio | no allocation, locks or logging | | prepare (via matrix.prepare) |
 * message, audio stopped | allocates |
 *
 * @par Lifetime
 * The matrix must outlive the controller. The controller cannot be moved (the
 * matrix holds pointers to its outputs).
 */
class TANH_API XYController {
public:
    /// Message thread. Registers 3 sources per voice with @p matrix.
    XYController(ModulationMatrix& matrix, XYControllerConfig config);
    /// Message thread. Removes the owned routings, then the sources (waits for the audio thread).
    ~XYController();

    XYController(const XYController&) = delete;
    XYController& operator=(const XYController&) = delete;
    XYController(XYController&&) = delete;
    XYController& operator=(XYController&&) = delete;

    [[nodiscard]] const XYControllerConfig& config() const { return m_config; }
    [[nodiscard]] uint32_t num_voices() const { return m_num_voices; }
    [[nodiscard]] std::string source_id(XYPadAxis axis, uint32_t voice = 0) const;
    [[nodiscard]] ModulationSource& source(XYPadAxis axis, uint32_t voice = 0);

    /// Allocate for blocks of up to @p max_block_size (message thread, audio
    /// stopped). The matrix calls it through the outputs; idempotent per (rate, size).
    void prepare(double sample_rate, size_t max_block_size);

    // ── Routing (message thread) ────────────────────────────────────────────

    /// Route one voice's axis to @p target_id. Returns the routing id (0 if rejected).
    uint32_t route(XYPadAxis axis,
                   std::string_view target_id,
                   const XYRouteOptions& options = {},
                   uint32_t voice = 0);
    /// Route voice v's axis to targets[v] for every v < min(num_voices, targets.size()).
    std::vector<uint32_t> route_voices(XYPadAxis axis,
                                       std::span<const std::string> targets,
                                       const XYRouteOptions& options = {});
    /// Remove a routing this controller owns. False for any other id.
    bool unroute(uint32_t routing_id);
    void unroute_all();
    /// ModulationMatrix::set_routing_enabled for an owned routing (no rebuild).
    bool set_route_enabled(uint32_t routing_id, bool enabled);
    bool set_route_depth(uint32_t routing_id, float depth);
    [[nodiscard]] std::span<const uint32_t> routes() const { return m_routes; }

    // ── Input (one UI thread; lock-free, allocation-free) ──────────────────

    /// Touch down (new id: grabs a voice) or move (known id). False for NaN/inf,
    /// a full touch table, no enabled voice, or a pad queue that is full.
    bool touch(TouchId id, float x, float y);
    /// Touch down on a given voice (no nearest-dot selection) or move.
    bool touch_voice(uint32_t voice, TouchId id, float x, float y);
    /// Touch up / cancel. False for an unknown id.
    bool release(TouchId id);
    /// Release every touch (unmount, focus loss, mode switch).
    void release_all();
    /// Re-send moves / releases a full queue coalesced. True when nothing is pending.
    bool flush();
    /// The voice a held touch drives (k_no_voice if not held).
    [[nodiscard]] uint32_t voice_of(TouchId id) const;
    static constexpr uint32_t k_no_voice = 0xFFFFFFFFu;

    /// Voices that are disabled (inactive effect slot) are never grabbed by touch().
    void set_voice_enabled(uint32_t voice, bool enabled);
    [[nodiscard]] bool voice_enabled(uint32_t voice) const;
    /// Kaoss-style hold: active stays 1 after a touch is released (until latch is turned off).
    void set_latch(bool latch) { m_latch.store(latch, std::memory_order_relaxed); }
    [[nodiscard]] bool latch() const { return m_latch.load(std::memory_order_relaxed); }

    // ── Motion recording (MotionRecorder threading contract) ───────────────

    [[nodiscard]] MotionRecorder& recorder(uint32_t voice = 0);
    /// MotionRecorder::service() for every voice (message thread timer, ≥ 10 Hz).
    bool service();

    // ── UI snapshot (one UI thread) ────────────────────────────────────────

    /// Copy the newest frame into @p out. False (out untouched) if nothing new.
    bool read_frame(XYFrame& out);
    /// Pop live recording trail points (oldest first). Returns the count.
    size_t drain_trail(std::span<XYPathPoint> out);
    /// While false the audio thread publishes no frames and no trail points.
    void set_ui_attached(bool attached) {
        m_ui_attached.store(attached, std::memory_order_relaxed);
    }
    /// Call @p f with voice @p voice's published lane (RCU read). Compare
    /// XYVoiceFrame::m_path_version to know when to call it.
    template <typename F>
    void read_path(uint32_t voice, F&& f) const {
        m_voices[voice]->m_recorder.read_lane(std::forward<F>(f));
    }

    // ── Audio thread ───────────────────────────────────────────────────────

    /// The block's transport (m_num_samples = block length). Call before
    /// matrix.process() every block.
    void set_transport(const thl::dsp::transport::TransportInfo& transport) noexcept
        TANH_NONBLOCKING_FUNCTION {
        m_transport = transport;
        m_transport_fresh = true;
    }
    /// set_transport() and run the driver now (no matrix, or a pre-matrix render).
    void process_block(const thl::dsp::transport::TransportInfo& transport) noexcept
        TANH_NONBLOCKING_FUNCTION;
    /// Host reset: the next block is treated as a timeline reset (recorders
    /// re-lock without a glide) and latched gates close.
    void reset() noexcept TANH_NONBLOCKING_FUNCTION;

    /// Output of the last driven block for one voice.
    [[nodiscard]] const float* out_x(uint32_t voice) const noexcept TANH_NONBLOCKING_FUNCTION;
    [[nodiscard]] const float* out_y(uint32_t voice) const noexcept TANH_NONBLOCKING_FUNCTION;
    [[nodiscard]] const uint8_t* out_active(uint32_t voice) const noexcept
        TANH_NONBLOCKING_FUNCTION;
    [[nodiscard]] const XYLayer* out_layer(uint32_t voice) const noexcept TANH_NONBLOCKING_FUNCTION;
    [[nodiscard]] std::span<const uint32_t> change_points(uint32_t voice) const noexcept
        TANH_NONBLOCKING_FUNCTION;
    [[nodiscard]] uint32_t num_samples() const noexcept TANH_NONBLOCKING_FUNCTION {
        return m_num_samples;
    }
    /// Driver runs (diagnostics, tests).
    [[nodiscard]] uint64_t blocks_driven() const noexcept TANH_NONBLOCKING_FUNCTION {
        return m_blocks_driven;
    }
    /// Blocks driven by the matrix without a fresh set_transport() (engine bug).
    [[nodiscard]] uint64_t stale_transport_blocks() const noexcept TANH_NONBLOCKING_FUNCTION {
        return m_stale_transport_blocks;
    }

private:
    friend class XYControllerOutput;

    struct Voice {
        Voice(XYController& owner, uint32_t index, const XYPadConfig& pad_config);
        XYPad m_pad;
        MotionRecorder m_recorder;
        // Audio thread: block buffers.
        std::vector<uint8_t> m_active;
        std::vector<uint8_t> m_xy_mask;
        std::vector<XYLayer> m_layer;
        std::vector<uint32_t> m_cps;
        size_t m_num_cps = 0;
        bool m_has_value = false;
        bool m_latched = false;
        uint32_t m_trail_countdown = 0;
        // Shared.
        std::atomic<uint64_t> m_position{0};  // packed x / y floats at block end (touch selection)
        std::atomic<bool> m_enabled{true};
        XYControllerOutput m_out_x;
        XYControllerOutput m_out_y;
        XYControllerOutput m_out_active;
    };

    struct UiTouch {
        TouchId m_id = 0;
        uint32_t m_voice = k_no_voice;
    };

    void drive_from_matrix() noexcept TANH_NONBLOCKING_FUNCTION;
    void run(const thl::dsp::transport::TransportInfo& t) noexcept TANH_NONBLOCKING_FUNCTION;
    void run_chunk(const thl::dsp::transport::TransportInfo& t) noexcept TANH_NONBLOCKING_FUNCTION;
    void run_voice(Voice& v,
                   uint32_t index,
                   const thl::dsp::transport::TransportInfo& t,
                   uint32_t n,
                   bool latch,
                   bool latch_changed,
                   bool ui) noexcept TANH_NONBLOCKING_FUNCTION;
    void publish_frame(uint32_t n) noexcept TANH_NONBLOCKING_FUNCTION;
    [[nodiscard]] uint32_t pick_voice(float x, float y) const;
    [[nodiscard]] uint32_t find_touch(TouchId id) const;

    ModulationMatrix& m_matrix;
    XYControllerConfig m_config;
    uint32_t m_num_voices = 1;
    std::vector<std::unique_ptr<Voice>> m_voices;

    // Message thread.
    std::vector<uint32_t> m_routes;

    // UI thread.
    std::array<UiTouch, k_xy_controller_max_touches> m_ui_touches{};

    // Shared.
    std::atomic<bool> m_latch{false};
    std::atomic<bool> m_ui_attached{true};
    TripleBuffer<XYFrame> m_frames;
    thl::core::LockFreeQueue<XYPathPoint, k_xy_trail_capacity> m_trail;

    // Audio thread.
    thl::dsp::transport::TransportInfo m_transport;
    bool m_transport_fresh = false;
    bool m_block_pending = false;
    bool m_driven_standalone = false;
    bool m_reset_pending = false;
    bool m_last_latch = false;
    uint32_t m_num_samples = 0;
    uint64_t m_sample_time = 0;
    uint64_t m_blocks_driven = 0;
    uint64_t m_stale_transport_blocks = 0;
    uint64_t m_frame_sequence = 0;
    uint32_t m_trail_dropped = 0;
    uint32_t m_frame_interval = 200;
    uint32_t m_since_publish = 0;
    uint64_t m_last_signature = 0;  // state of the last published frame (publish at once on change)

    double m_sample_rate = 0.0;
    size_t m_capacity = 0;
};

/// Pad mode of a multi-slot effect: one pad for all slots, or one per slot.
enum class XYPadMode : uint8_t { Single, PerEffect };

/**
 * @brief Single / PerEffect wiring over XY controllers, switched without a
 *        schedule rebuild.
 *
 * Every target gets two routings: one from a PerEffect controller voice
 * (enabled in PerEffect mode) and one from a Single controller voice (enabled
 * in Single mode). set_mode() flips all of them in one
 * ModulationMatrix::set_routings_enabled() batch, so no audio block sees
 * neither or both sets. The routings are owned by the controllers (their
 * destructors remove them); the router only keeps ids.
 */
class TANH_API XYModeRouter {
public:
    explicit XYModeRouter(ModulationMatrix& matrix, XYPadMode mode = XYPadMode::PerEffect);

    /// Message thread. Route @p target ← per_effect voice and ← single voice.
    /// Default priorities: PerEffect 10, Single 20. False if either routing was rejected.
    bool add_target(std::string_view target_id,
                    XYPadAxis axis,
                    XYController& per_effect,
                    uint32_t per_effect_voice,
                    XYController& single,
                    uint32_t single_voice);
    bool add_target(std::string_view target_id,
                    XYPadAxis axis,
                    XYController& per_effect,
                    uint32_t per_effect_voice,
                    XYController& single,
                    uint32_t single_voice,
                    XYRouteOptions per_effect_options,
                    XYRouteOptions single_options);

    /// Message thread. Lock-free for the audio thread, no rebuild.
    void set_mode(XYPadMode mode);
    [[nodiscard]] XYPadMode mode() const { return m_mode; }
    [[nodiscard]] size_t num_targets() const { return m_per_effect.size(); }

private:
    ModulationMatrix& m_matrix;
    XYPadMode m_mode;
    std::vector<uint32_t> m_per_effect;
    std::vector<uint32_t> m_single;
    std::vector<RoutingEnabled> m_batch;
};

}  // namespace thl::modulation
