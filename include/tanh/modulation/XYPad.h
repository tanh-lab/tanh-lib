#pragma once

#include <tanh/core/Exports.h>
#include <tanh/core/threading/LockFreeQueue.h>
#include <tanh/modulation/ModulationSource.h>
#include <tanh/state/ModulationScope.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace thl::modulation {

class ModulationMatrix;
class XYPad;

/// Identifies one finger for its lifetime (JUCE MouseInputSource::getIndex(), a
/// UITouch pointer, a React Native touch identifier).
using TouchId = uint64_t;

/// Compile-time cap on touches per pad (fixed arrays, no allocation on touch).
inline constexpr uint32_t k_xy_pad_max_touches = 16;

/// Capacity of the UI → audio event queue of one pad.
inline constexpr size_t k_xy_pad_queue_capacity = 256;

/// Global-scope pads with several touches: which touch drives the single output.
enum class MonoPriority : uint8_t {
    Last,  ///< the most recent touch; releasing it falls back to the newest remaining one
    First  ///< the oldest touch; later touches only take over when it is released
};

struct XYPadConfig {
    /// k_global_scope: one mono output stream (touches reduced by m_mono_priority).
    /// Any other scope: one stream per touch slot, slot = voice.
    ModulationScope m_scope = k_global_scope;
    /// Touches tracked at once, clamped to [1, k_xy_pad_max_touches]. Surplus
    /// fingers are ignored until released.
    uint32_t m_max_touches = 1;
    MonoPriority m_mono_priority = MonoPriority::Last;
};

enum class XYPadAxis : uint8_t { X, Y, Active };

namespace detail {

enum class XYPadEventKind : uint8_t { Down, Move, Up };

/// One UI → audio pad event; x and y travel together.
struct XYPadEvent {
    uint8_t m_stream = 0;
    XYPadEventKind m_kind = XYPadEventKind::Move;
    float m_x = 0.0f;
    float m_y = 0.0f;
};

/// Events a stream could not queue yet, in send order: pre (down/move), up, post (down).
struct XYPadPending {
    float m_pre_x = 0.0f, m_pre_y = 0.0f;
    float m_post_x = 0.0f, m_post_y = 0.0f;
    float m_up_x = 0.0f, m_up_y = 0.0f;
    bool m_pre = false;
    bool m_pre_down = false;
    bool m_up = false;
    bool m_post = false;
    [[nodiscard]] bool any() const { return m_pre || m_up || m_post; }
};

}  // namespace detail

/**
 * @brief One pad stream for the block last rendered by XYPad::process_block().
 *
 * m_x / m_y hold the position (x right, y up, both in [0, 1]); they keep the
 * last position after a release (an up carries the touch's final position). m_active is 1 while the
 * touch is down and is also the active mask of all three matrix outputs. m_change_points lists the
 * sorted, unique offsets at which an event was applied (shared by x, y, active).
 */
struct XYPadStream {
    const float* m_x = nullptr;
    const float* m_y = nullptr;
    const uint8_t* m_active = nullptr;
    std::span<const uint32_t> m_change_points;
    uint32_t m_num_samples = 0;
};

/**
 * @brief One of the three matrix sources of an XYPad (x, y or active).
 *
 * Owned by its XYPad. pre_process_block() makes sure the pad rendered this block
 * (once, whichever output runs first) and copies the axis into its buffers.
 */
class TANH_API XYPadOutput final : public ModulationSource {
public:
    XYPadOutput(XYPad& pad, XYPadAxis axis);

    void prepare(double sample_rate, size_t samples_per_block, uint32_t voice_count) override;
    void pre_process_block() override;

    [[nodiscard]] XYPadAxis axis() const { return m_axis; }

private:
    void copy_stream(const XYPadStream& s, float* out, uint8_t* mask) const;

    XYPad& m_pad;
    const XYPadAxis m_axis;
};

/**
 * @class XYPad
 * @brief Turns UI touches into sample-accurate x / y / active modulation.
 *
 * The UI thread calls touch() / release(); a single lock-free queue carries x and
 * y together, so a diagonal move changes both axes at the same in-block offset.
 * On the audio thread process_block() drains the queue once, spreads each
 * stream's events over the block (detail::spread_offset, the InputEventQueue
 * semantics) and writes per-stream x / y / active buffers. Values and the gate
 * hold across blocks until the next event.
 *
 * Two ways to use it:
 * - **As matrix sources**: add_to(matrix, "pad") registers "pad.x", "pad.y" and
 *   "pad.active". The outputs render the pad in their pre_process_block(), once
 *   per block. Route x/y with ReplaceHold and active with Replace.
 * - **Driven by an owner** (an XY controller with recorder and sequencer): the
 *   owner calls process_block(n) on the audio thread — for example from its own
 *   source's pre_process_block() — and reads stream() / primary(). If the pad's
 *   outputs are also in the matrix, the matrix then reuses that render instead of
 *   draining again (detected through the matrix's processed-sample counter).
 *   Calling process_block(n) with the real block size before matrix.process(n)
 *   is also how a variable-block host gets offsets spread over the real block;
 *   without it the outputs spread over the prepared maximum block size.
 *
 * Coordinates are normalised by the caller with y pointing up (bottom = 0), then
 * clamped to [0, 1]; NaN/inf is rejected.
 *
 * @par Threading
 * | Call | Thread | Guarantees |
 * |---|---|---|
 * | touch, release, release_all, flush, touches | one UI thread | O(max_touches), ≤ 3 try_push, no
 * alloc/locks | | ctor, add_to, remove_from, prepare | setup / message, audio stopped for prepare |
 * may allocate | | process_block, stream, primary, outputs' pre_process_block | audio | ≤ queue
 * capacity pops, O(events + block), no alloc |
 *
 * @par Queue full
 * If the audio thread stops draining, moves are coalesced per stream (latest
 * wins) and re-sent by flush() or the next UI call; a release that cannot be sent
 * is kept pending and sent before anything newer, and a new touch that cannot be
 * sent claims no slot (touch() returns false). No gate edge is lost: after the
 * queue drains and flush() runs, the audio side ends in the UI's final state.
 */
class TANH_API XYPad {
public:
    struct TouchView {
        TouchId m_id = 0;
        float m_x = 0.0f;
        float m_y = 0.0f;
        bool m_active = false;
    };

    explicit XYPad(XYPadConfig config = {});
    ~XYPad();

    XYPad(const XYPad&) = delete;
    XYPad& operator=(const XYPad&) = delete;
    XYPad(XYPad&&) = delete;
    XYPad& operator=(XYPad&&) = delete;

    // ── Setup / message thread ────────────────────────────────────────────

    /// Register "<prefix>.x", "<prefix>.y" and "<prefix>.active" with @p matrix.
    /// One matrix per pad; call remove_from() before the pad or matrix dies.
    void add_to(ModulationMatrix& matrix, std::string_view prefix);
    /// Unregister the three sources (blocks until the audio thread let go).
    void remove_from(ModulationMatrix& matrix);

    /// Allocate the stream buffers for blocks of up to @p max_block_size samples.
    /// Needed only when the pad is driven without a matrix; the outputs call it
    /// from the matrix's prepare(). Never shrinks.
    void prepare(size_t max_block_size);

    [[nodiscard]] const XYPadConfig& config() const { return m_config; }
    [[nodiscard]] uint32_t max_touches() const { return m_config.m_max_touches; }
    /// 1 for a global pad, max_touches() otherwise.
    [[nodiscard]] uint32_t num_streams() const { return m_num_streams; }
    [[nodiscard]] ModulationSource& source(XYPadAxis axis);
    /// "<prefix>.x" etc. (empty before add_to()).
    [[nodiscard]] std::string source_id(XYPadAxis axis) const;

    // ── UI thread (single producer): lock-free, allocation-free ─────────────

    /// Touch down (new id) or move (known id). Returns false for NaN/inf, when
    /// every slot is taken, or when a new touch cannot be queued.
    bool touch(TouchId id, float x, float y);
    /// Touch up or cancel. Returns false for an unknown id.
    bool release(TouchId id);
    /// Release every touch (unmount, focus loss, app backgrounded).
    void release_all();
    /// Re-send coalesced moves / pending releases. True when nothing is pending.
    bool flush();
    /// The UI-side slot table (max_touches() entries) for drawing.
    [[nodiscard]] std::span<const TouchView> touches() const;

    // ── Audio thread ───────────────────────────────────────────────────────

    /// Drain the queue and render @p num_samples (≤ prepared size) for every stream.
    void process_block(uint32_t num_samples) noexcept TANH_NONBLOCKING_FUNCTION;

    /// Stream @p index of the last rendered block.
    [[nodiscard]] XYPadStream stream(uint32_t index) const noexcept TANH_NONBLOCKING_FUNCTION;
    /// The stream of the oldest held touch (a global pad: stream 0). With no
    /// touch held, the stream released last.
    [[nodiscard]] XYPadStream primary() const noexcept TANH_NONBLOCKING_FUNCTION;
    [[nodiscard]] uint32_t primary_index() const noexcept TANH_NONBLOCKING_FUNCTION {
        return m_primary;
    }
    /// Frames of the last process_block().
    [[nodiscard]] uint32_t rendered_frames() const noexcept TANH_NONBLOCKING_FUNCTION {
        return m_rendered_frames;
    }

private:
    friend class XYPadOutput;

    using Kind = detail::XYPadEventKind;
    using PadEvent = detail::XYPadEvent;
    using Pending = detail::XYPadPending;

    static constexpr uint32_t k_no_slot = 0xFFFFFFFFu;

    // UI side
    bool emit_pos(uint32_t stream, float x, float y, bool down);
    void emit_up(uint32_t stream, float x, float y);
    bool flush_stream(uint32_t stream);
    [[nodiscard]] uint32_t find_slot(TouchId id) const;
    [[nodiscard]] uint32_t pick_driver() const;

    // Audio side
    void render_for_matrix(uint32_t num_samples) noexcept TANH_NONBLOCKING_FUNCTION;
    void apply(const PadEvent& e, uint32_t offset, uint32_t n) noexcept TANH_NONBLOCKING_FUNCTION;

    XYPadConfig m_config;
    uint32_t m_num_streams = 1;

    thl::core::LockFreeQueue<PadEvent, k_xy_pad_queue_capacity> m_queue;

    // UI thread only
    std::array<TouchView, k_xy_pad_max_touches> m_ui_slots{};
    std::array<uint64_t, k_xy_pad_max_touches> m_ui_press_seq{};
    std::array<Pending, k_xy_pad_max_touches> m_pending{};
    uint64_t m_press_counter = 0;
    uint32_t m_driver = k_no_slot;  // global pads: the slot that drives stream 0

    // Audio thread only
    std::array<PadEvent, k_xy_pad_queue_capacity> m_scratch{};
    std::array<float, k_xy_pad_max_touches> m_last_x{};
    std::array<float, k_xy_pad_max_touches> m_last_y{};
    std::array<uint8_t, k_xy_pad_max_touches> m_last_active{};
    std::array<uint64_t, k_xy_pad_max_touches> m_down_seq{};
    std::array<uint32_t, k_xy_pad_max_touches> m_cp_count{};
    uint64_t m_audio_down_counter = 0;
    uint32_t m_primary = 0;
    uint32_t m_rendered_frames = 0;
    uint64_t m_render_stamp = 0;
    bool m_rendered_for_matrix = false;

    // Stream buffers: stream s at s * m_capacity (allocated in prepare()).
    std::vector<float> m_x_storage;
    std::vector<float> m_y_storage;
    std::vector<uint8_t> m_active_storage;
    std::vector<uint32_t> m_cp_storage;
    size_t m_capacity = 0;

    std::atomic<const ModulationMatrix*> m_matrix{nullptr};
    std::string m_prefix;

    XYPadOutput m_out_x;
    XYPadOutput m_out_y;
    XYPadOutput m_out_active;
};

}  // namespace thl::modulation
