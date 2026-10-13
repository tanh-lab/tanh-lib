#pragma once

#include <tanh/core/Exports.h>
#include <tanh/dsp/transport/TransportInfo.h>
#include <tanh/modulation/BlockProcessor.h>
#include <tanh/modulation/ModulationMatrix.h>
#include <tanh/modulation/ModulationRouting.h>
#include <tanh/modulation/ModulationSource.h>
#include <tanh/modulation/MotionRecorder.h>
#include <tanh/modulation/XYFrame.h>
#include <tanh/modulation/XYTypes.h>
#include <tanh/modulation/detail/XYFramePublisher.h>
#include <tanh/modulation/detail/XYTouchTable.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace thl::modulation {

struct XYControllerConfig {
    /// Source prefix. One voice: "<id>.x", "<id>.y", "<id>.active". Several
    /// voices: "<id>.<v>.x" and so on.
    std::string m_id;
    /// Dots driven by this controller, clamped to [1, k_xy_controller_max_voices].
    uint32_t m_num_voices = 1;
    /// Fingers per voice; m_mono_priority picks the one that drives it.
    uint32_t m_max_touches = 10;
    MonoPriority m_mono_priority = MonoPriority::Last;
    /// Configuration of every voice's recorder.
    MotionRecorderConfig m_recorder;
    /// UI frames are published at most at this rate, and at once on a state change.
    double m_frame_rate_hz = 240.0;
    /// Samples between live trail points while a voice records.
    uint32_t m_trail_interval = 32;
    /// Delay of timestamped touches, so they land ahead of the block that plays
    /// them. Unset: one prepared block plus 5 ms. x / y ramp over the whole gap
    /// between two moves only while the delay exceeds one block plus that gap.
    std::optional<double> m_input_delay_ms;
};

/// Options of XYController::route(). The defaults are those of ModulationRouting.
struct XYRouteOptions {
    /// Default: ReplaceHold for X and Y, Replace for Active.
    std::optional<CombineMode> m_combine;
    uint32_t m_priority = 0;
    std::optional<uint32_t> m_hold_priority;
    float m_depth = 1.0f;
    /// Replace range in the target's normalized [0, 1] units.
    std::optional<std::pair<float, float>> m_range_normalized;
    bool m_enabled = true;
    uint32_t m_max_decimation = 0;
    /// Leave the target alone while it is in a host gesture (a knob held), so the knob wins.
    bool m_skip_during_gesture = false;
};

/**
 * @brief Touch input, motion recording and a UI snapshot for one or more XY
 *        dots, exposed as matrix sources.
 *
 * Every voice (dot) has a touch pad and a MotionRecorder and registers three
 * matrix sources: x, y and active. The controller is a BlockProcessor of the
 * matrix: it renders every voice once per block, before the sources copy their
 * buffers out, synced to the transport the engine passes to
 * ModulationMatrix::process() (free-running without one).
 *
 * With several voices, a new touch grabs the nearest enabled voice that no
 * other finger holds and moves only that voice until it is released.
 *
 * Threading:
 * - ctor, dtor, route, unroute, set_route_*, service: message thread.
 * - touch, touch_voice, release, release_all, flush, read_frame, drain_trail,
 *   read_path: one UI thread, lock-free.
 * - set_latch, set_voice_enabled, set_ui_attached: any thread.
 * - recorder(v): as documented on MotionRecorder.
 * - set_block_time, reset and the out_* getters: audio thread.
 *
 * The matrix must outlive the controller.
 */
class TANH_API XYController {
public:
    static constexpr uint32_t k_no_voice = k_xy_no_voice;

    /// Registers three sources per voice with @p matrix.
    XYController(ModulationMatrix& matrix, XYControllerConfig config);
    /// Removes the owned routings, then the sources (waits for the audio thread).
    ~XYController();

    XYController(const XYController&) = delete;
    XYController& operator=(const XYController&) = delete;
    XYController(XYController&&) = delete;
    XYController& operator=(XYController&&) = delete;

    [[nodiscard]] const XYControllerConfig& config() const { return m_config; }
    [[nodiscard]] uint32_t num_voices() const { return m_num_voices; }
    [[nodiscard]] std::string source_id(XYOutput axis, uint32_t voice = 0) const;
    [[nodiscard]] ModulationSource& source(XYOutput axis, uint32_t voice = 0);

    /// Route one voice's axis to @p target_id. Returns the routing id, or
    /// k_invalid_routing_id if the matrix rejected it.
    uint32_t route(XYOutput axis,
                   std::string_view target_id,
                   const XYRouteOptions& options = {},
                   uint32_t voice = 0);
    /// Remove a routing this controller owns. False for any other id.
    bool unroute(uint32_t routing_id);
    void unroute_all();
    /// Enable or disable an owned routing without a schedule rebuild.
    bool set_route_enabled(uint32_t routing_id, bool enabled);
    bool set_route_depth(uint32_t routing_id, float depth);
    [[nodiscard]] std::span<const uint32_t> routes() const { return m_routes; }

    /// Touch down (a new id grabs a voice) or move (a known id). False for a
    /// non-finite position, a full touch table, no enabled voice or a full queue.
    /// @p time_ns is when the touch happened, on the clock of set_block_time(); with it
    /// (and set_block_time()) the touch plays at its own sample, and x / y ramp
    /// between timed moves. Without it the block's touches are spread and held.
    bool touch(TouchId id, float x, float y, std::optional<int64_t> time_ns = std::nullopt);
    /// Touch down on a given voice without nearest-dot selection, or move.
    bool touch_voice(uint32_t voice,
                     TouchId id,
                     float x,
                     float y,
                     std::optional<int64_t> time_ns = std::nullopt);
    /// Touch up or cancel. False for an unknown id.
    bool release(TouchId id, std::optional<int64_t> time_ns = std::nullopt);
    /// Release every touch (unmount, focus loss, mode switch).
    void release_all();
    /// Re-send moves and releases that a full queue coalesced. True when nothing is pending.
    bool flush();
    /// The voice a held touch drives, or k_no_voice.
    [[nodiscard]] uint32_t voice_of(TouchId id) const;

    /// A disabled voice is never grabbed by touch().
    void set_voice_enabled(uint32_t voice, bool enabled);
    [[nodiscard]] bool voice_enabled(uint32_t voice) const;
    /// While latched, active stays 1 after a touch is released.
    void set_latch(bool latch) { m_latch.store(latch, std::memory_order_relaxed); }
    [[nodiscard]] bool latch() const { return m_latch.load(std::memory_order_relaxed); }

    [[nodiscard]] MotionRecorder& recorder(uint32_t voice = 0);
    /// MotionRecorder::service() for every voice (message-thread timer, 10 Hz or more).
    bool service();
    /// Calls @p f with every voice's recorder, e.g. to set smoothing on all of them.
    template <typename F>
    void for_each_recorder(F&& f) {
        for (uint32_t v = 0; v < m_num_voices; ++v) { f(recorder(v)); }
    }

    /// Copy the newest frame into @p out. False, with @p out untouched, if nothing is new.
    bool read_frame(XYFrame& out);
    /// Pop live recording trail points, oldest first. Returns the count.
    size_t drain_trail(std::span<XYPathPoint> out);
    /// While false the audio thread publishes no frames and no trail points.
    void set_ui_attached(bool attached) {
        m_ui_attached.store(attached, std::memory_order_relaxed);
    }
    /// Call @p f with the published lane of @p voice (an RCU read).
    template <typename F>
    void read_path(uint32_t voice, F&& f) const {
        voice_recorder(voice).read_lane(std::forward<F>(f));
    }

    /// The time of the block's first sample, on the clock of the touch timestamps
    /// (any monotonic nanosecond clock, e.g. thl::core::uptime_ns()). Call before
    /// every matrix.process() to place timestamped touches; it holds for that call
    /// only. A host block split into chunks needs one call per chunk: the callback
    /// time plus the chunk's offset at the sample rate. A block without it spreads
    /// its touches as if untimed.
    void set_block_time(int64_t now_ns) TANH_NONBLOCKING_FUNCTION;

    /// Host reset: the next block re-locks playback without a glide and latched gates close.
    void reset() TANH_NONBLOCKING_FUNCTION;

    /// Output of the last driven block.
    [[nodiscard]] const float* out_x(uint32_t voice) const TANH_NONBLOCKING_FUNCTION;
    [[nodiscard]] const float* out_y(uint32_t voice) const TANH_NONBLOCKING_FUNCTION;
    [[nodiscard]] const uint8_t* out_active(uint32_t voice) const TANH_NONBLOCKING_FUNCTION;
    [[nodiscard]] const XYLayer* out_layer(uint32_t voice) const TANH_NONBLOCKING_FUNCTION;
    [[nodiscard]] std::span<const uint32_t> change_points(uint32_t voice) const
        TANH_NONBLOCKING_FUNCTION;
    [[nodiscard]] uint32_t num_samples() const TANH_NONBLOCKING_FUNCTION { return m_num_samples; }
    /// Number of controller runs since construction.
    [[nodiscard]] uint64_t blocks_driven() const TANH_NONBLOCKING_FUNCTION {
        return m_blocks_driven;
    }

private:
    class Output;
    struct Voice;

    // The matrix's per-block hook (prepare and render).
    class MatrixHook final : public BlockProcessor {
    public:
        explicit MatrixHook(XYController& controller) : m_controller(controller) {}
        void prepare(double sample_rate, size_t samples_per_block) override {
            m_controller.prepare(sample_rate, samples_per_block);
        }
        void process_block(const BlockContext& context) override {
            m_controller.process_block(context);
        }

    private:
        XYController& m_controller;
    };

    [[nodiscard]] const MotionRecorder& voice_recorder(uint32_t voice) const;
    // Allocate for blocks of up to @p samples_per_block and abort running takes
    // (from the matrix's prepare(), audio stopped).
    void prepare(double sample_rate, size_t samples_per_block);
    // One matrix block.
    void process_block(const BlockContext& context) TANH_NONBLOCKING_FUNCTION;
    void run(const thl::dsp::transport::TransportInfo& transport) TANH_NONBLOCKING_FUNCTION;
    void run_voice(Voice& voice,
                   uint32_t index,
                   const thl::dsp::transport::TransportInfo& transport,
                   bool gate_may_close,
                   bool ui) TANH_NONBLOCKING_FUNCTION;
    [[nodiscard]] uint32_t pick_voice(float x, float y) const;

    // Shared between threads: the UI side (frames and trail).
    detail::XYFramePublisher m_frames;

    ModulationMatrix& m_matrix;
    XYControllerConfig m_config;
    std::vector<std::unique_ptr<Voice>> m_voices;

    // Message thread.
    std::vector<uint32_t> m_routes;

    // UI thread.
    detail::XYTouchTable m_touches;

    MatrixHook m_hook{*this};

    // Audio thread.
    int64_t m_block_time_ns = 0;
    std::optional<int64_t> m_run_time_ns;  // clock time of the running block (timed touches)
    double m_sample_rate = 0.0;
    uint64_t m_sample_time = 0;
    uint64_t m_blocks_driven = 0;
    size_t m_capacity = 0;
    uint32_t m_num_samples = 0;
    uint32_t m_num_voices = 1;  // set at construction

    // Shared between threads.
    std::atomic<bool> m_latch{false};
    std::atomic<bool> m_ui_attached{true};

    // Audio thread.
    bool m_reset_pending = false;
    bool m_block_time_fresh = false;
    bool m_last_latch = false;
};

}  // namespace thl::modulation
