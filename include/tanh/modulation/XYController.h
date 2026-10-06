#pragma once

#include <tanh/core/Exports.h>
#include <tanh/core/threading/LockFreeQueue.h>
#include <tanh/core/threading/TripleBuffer.h>
#include <tanh/dsp/transport/TransportInfo.h>
#include <tanh/modulation/ModulationMatrix.h>
#include <tanh/modulation/ModulationRouting.h>
#include <tanh/modulation/ModulationSource.h>
#include <tanh/modulation/MotionRecorder.h>
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

/// Identifies one finger for its lifetime (a JUCE mouse source index, a UITouch
/// pointer, a React Native touch identifier).
using TouchId = uint64_t;

/// Voices (dots) per controller.
inline constexpr uint32_t k_xy_controller_max_voices = 16;
/// Fingers tracked at once per controller.
inline constexpr uint32_t k_xy_controller_max_touches = 16;
/// Capacity of the live recording trail (audio to UI, drop-oldest).
inline constexpr size_t k_xy_trail_capacity = 2048;

/// Which of several fingers on one voice drives it.
enum class MonoPriority : uint8_t {
    Last,  ///< the most recent touch; releasing it falls back to the newest remaining one
    First  ///< the oldest touch; later touches only take over when it is released
};

/// The three matrix sources of a voice.
enum class XYPadAxis : uint8_t { X, Y, Active };

/// Which layer produced a voice's output. Touch has priority over Motion.
enum class XYLayer : uint8_t {
    None,    ///< no layer is active: x and y hold
    Touch,   ///< a live touch
    Motion,  ///< motion playback
};

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
};

/// One live recording trail point.
struct XYPathPoint {
    float m_x = 0.0f;
    float m_y = 0.0f;
    float m_progress = 0.0f;  ///< recording progress of the voice, 0..1
    uint8_t m_voice = 0;
    uint8_t m_active = 0;
};

/// One voice in an XYFrame. All fields come from the same audio block.
struct XYVoiceFrame {
    float m_x = 0.0f;  ///< output at block end
    float m_y = 0.0f;
    uint8_t m_active = 0;      ///< gate output, including the latch
    bool m_has_value = false;  ///< touched or played at least once
    bool m_touched = false;    ///< a live touch drives the voice
    XYLayer m_layer = XYLayer::None;
    uint32_t m_path_version = 0;  ///< MotionRecorder::lane_version(): re-read the path on change
    MotionSnapshot m_motion;      ///< the voice's recorder
};

/// Fixed-size UI snapshot of one controller, published once per block (rate
/// limited) through a TripleBuffer.
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
 * @brief Touch input, motion recording and a UI snapshot for one or more XY
 *        dots, exposed as matrix sources.
 *
 * Every voice (dot) has a touch pad and a MotionRecorder and registers three
 * matrix sources: x, y and active. The controller runs once per block, from the
 * first of its sources the matrix reaches, so the engine only calls
 * set_transport() before matrix.process(). Blocks larger than the prepared
 * size run as prepared-size chunks.
 *
 * With several voices, a new touch grabs the nearest enabled voice that no
 * other finger holds and moves only that voice until it is released.
 *
 * Threading:
 * - ctor, dtor, route, unroute, set_route_*, service, set_smoothing: message thread.
 * - touch, touch_voice, release, release_all, flush, read_frame, drain_trail,
 *   read_path: one UI thread, lock-free.
 * - set_latch, set_voice_enabled, set_ui_attached: any thread.
 * - recorder(v): as documented on MotionRecorder.
 * - set_transport, process_block, reset and the out_* getters: audio thread.
 *
 * The matrix must outlive the controller.
 */
class TANH_API XYController {
public:
    static constexpr uint32_t k_no_voice = 0xFFFFFFFFu;

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
    [[nodiscard]] std::string source_id(XYPadAxis axis, uint32_t voice = 0) const;
    [[nodiscard]] ModulationSource& source(XYPadAxis axis, uint32_t voice = 0);

    /// Allocate for blocks of up to @p max_block_size and abort running takes.
    /// The matrix calls it from its prepare(); the audio thread must be stopped.
    void prepare(double sample_rate, size_t max_block_size);

    /// Route one voice's axis to @p target_id. Returns the routing id, or
    /// k_invalid_routing_id if the matrix rejected it.
    uint32_t route(XYPadAxis axis,
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
    bool touch(TouchId id, float x, float y);
    /// Touch down on a given voice without nearest-dot selection, or move.
    bool touch_voice(uint32_t voice, TouchId id, float x, float y);
    /// Touch up or cancel. False for an unknown id.
    bool release(TouchId id);
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
    /// MotionRecorder::set_smoothing() for every voice (message thread).
    void set_smoothing(float amount);

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

    /// The block's transport (m_num_samples = block length). Call before
    /// matrix.process() every block.
    void set_transport(const thl::dsp::transport::TransportInfo& transport)
        TANH_NONBLOCKING_FUNCTION;
    /// Run the controller now for @p transport; the matrix's next pass reuses
    /// this render instead of running again.
    void process_block(const thl::dsp::transport::TransportInfo& transport)
        TANH_NONBLOCKING_FUNCTION;
    /// Host reset: the next block re-locks playback without a glide and latched gates close.
    void reset() TANH_NONBLOCKING_FUNCTION;

    /// Output of the last driven block (the last chunk of an oversized block).
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
    /// Blocks the matrix ran without a fresh set_transport() (an engine bug).
    [[nodiscard]] uint64_t stale_transport_blocks() const TANH_NONBLOCKING_FUNCTION {
        return m_stale_transport_blocks;
    }

private:
    class Output;
    struct Voice;

    struct UiTouch {
        TouchId m_id = 0;
        uint32_t m_voice = k_no_voice;
    };

    [[nodiscard]] const MotionRecorder& voice_recorder(uint32_t voice) const;
    void run_for_matrix();
    void run(const thl::dsp::transport::TransportInfo& transport);
    void run_chunk(const thl::dsp::transport::TransportInfo& transport);
    void run_voice(Voice& voice,
                   uint32_t index,
                   const thl::dsp::transport::TransportInfo& transport,
                   bool latch_changed,
                   bool ui);
    void publish_frame(uint32_t num_samples);
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

    // Shared between threads.
    std::atomic<bool> m_latch{false};
    std::atomic<bool> m_ui_attached{true};
    TripleBuffer<XYFrame> m_frames;
    thl::core::LockFreeQueue<XYPathPoint, k_xy_trail_capacity> m_trail;

    // Audio thread.
    thl::dsp::transport::TransportInfo m_transport;
    bool m_transport_fresh = false;
    bool m_matrix_block_pending = false;
    bool m_rendered_ahead = false;  // process_block() ran for the coming matrix block
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
    uint64_t m_last_signature = 0;  // discrete state of the last published frame

    size_t m_capacity = 0;
};

}  // namespace thl::modulation
