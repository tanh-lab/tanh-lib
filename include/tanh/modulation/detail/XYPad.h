#pragma once

#include <tanh/core/Exports.h>
#include <tanh/core/threading/LockFreeQueue.h>
#include <tanh/modulation/MotionRecorder.h>
#include <tanh/modulation/XYController.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace thl::modulation::detail {

/// Capacity of the UI to audio event queue of one pad.
inline constexpr size_t k_xy_pad_queue_capacity = 256;

/// Timestamp of an event that carries none.
inline constexpr int64_t k_xy_pad_untimed = std::numeric_limits<int64_t>::min();

enum class XYPadEventKind : uint8_t {
    Down,  ///< gate opens at the position
    Move,  ///< the driving finger moved
    Jump,  ///< another finger took over: the position jumps, the gate stays open
    Up     ///< gate closes, the position holds
};

/// One UI to audio pad event; x and y travel together.
struct XYPadEvent {
    XYPadEventKind m_kind = XYPadEventKind::Move;
    float m_x = 0.0f;
    float m_y = 0.0f;
    int64_t m_time_ns = k_xy_pad_untimed;
};

/// How a pad places timestamped events.
struct XYPadTiming {
    /// 0 disables timed placement: every event is spread as if untimed.
    double m_sample_rate = 0.0;
    /// Fixed delay added to every timestamp, so events land ahead of the block.
    int64_t m_delay_ns = 0;
    /// Change point spacing while x / y ramp between two timed moves.
    uint32_t m_ramp_interval = 32;
};

/**
 * @brief The touch input of one XYController voice: UI touches in, one
 *        sample-accurate x / y / active stream out.
 *
 * Several fingers reduce to one stream by MonoPriority. Coordinates are
 * clamped to [0, 1] with y pointing up. touch(), release() and flush() run on
 * one UI thread; prepare() with the audio thread stopped; process_block() and
 * output() on the audio thread. When the queue is full, moves are coalesced
 * and a release is kept pending, so no gate edge is lost once flush() runs.
 *
 * Untimed events are spread over the block they are drained in and held.
 * Timestamped events land at their own sample when process_block() gets the
 * block's clock time, and x / y ramp linearly between two timed moves.
 */
class TANH_API XYPad {
public:
    XYPad(uint32_t max_touches, MonoPriority priority);

    /// Allocate the stream for blocks of up to @p max_block_size samples.
    void prepare(size_t max_block_size, const XYPadTiming& timing = {});

    /// Touch down (new id) or move (known id). False for a non-finite position,
    /// a full touch table, or a new touch that cannot be queued.
    bool touch(TouchId id, float x, float y, int64_t time_ns = k_xy_pad_untimed);
    /// Touch up or cancel. False for an unknown id.
    bool release(TouchId id, int64_t time_ns = k_xy_pad_untimed);
    void release_all();
    /// Re-send coalesced events. True when nothing is pending.
    bool flush();

    /// Drain the queue and render @p num_samples (at most the prepared size).
    /// @p block_time_ns is the clock time of this block (see XYController::set_block_time()).
    void process_block(uint32_t num_samples, std::optional<int64_t> block_time_ns = std::nullopt)
        TANH_NONBLOCKING_FUNCTION;
    /// The stream of the last process_block().
    [[nodiscard]] MotionInput output() const TANH_NONBLOCKING_FUNCTION;

private:
    static constexpr uint32_t k_no_slot = 0xFFFFFFFFu;

    struct Slot {
        TouchId m_id = 0;
        float m_x = 0.0f;
        float m_y = 0.0f;
        bool m_held = false;
        uint64_t m_press_order = 0;
    };

    // Events that could not be queued yet, in send order: position (down, move
    // or jump), up, then a down that followed the up.
    struct Pending {
        std::optional<XYPadEvent> m_position;
        std::optional<XYPadEvent> m_up;
        std::optional<XYPadEvent> m_down_after_up;
        [[nodiscard]] bool any() const {
            return m_position.has_value() || m_up.has_value() || m_down_after_up.has_value();
        }
    };

    // An event placed on the pad's sample timeline.
    struct Scheduled {
        XYPadEvent m_event;
        int64_t m_sample = 0;
        bool m_timed = false;  // placed by its timestamp
    };

    bool send_position(XYPadEventKind kind, float x, float y, int64_t time_ns);
    void send_up(float x, float y, int64_t time_ns);
    [[nodiscard]] uint32_t find_slot(TouchId id) const;
    [[nodiscard]] uint32_t pick_driver() const;
    void drain(uint32_t num_samples, std::optional<int64_t> block_time_ns);
    void start_ramp(int64_t from_sample);
    void render(uint32_t from, uint32_t to);
    void apply(const XYPadEvent& event, uint32_t offset);
    void add_change_point(uint32_t offset);

    uint32_t m_max_touches = 1;
    MonoPriority m_priority = MonoPriority::Last;
    thl::core::LockFreeQueue<XYPadEvent, k_xy_pad_queue_capacity> m_queue;

    // UI thread only.
    std::array<Slot, k_xy_controller_max_touches> m_slots{};
    Pending m_pending;
    uint64_t m_press_counter = 0;
    uint32_t m_driver = k_no_slot;  // the slot that drives the stream

    // Audio thread only.
    XYPadTiming m_timing;
    int64_t m_max_wait = 0;  // latest offset a timed event may land at
    // Drained events not yet applied, oldest first (a ring).
    std::array<Scheduled, k_xy_pad_queue_capacity> m_waiting{};
    size_t m_waiting_head = 0;
    size_t m_waiting_count = 0;
    int64_t m_block_start = 0;     // pad sample of this block's first sample
    int64_t m_last_scheduled = 0;  // pad sample of the newest scheduled event
    float m_current_x = 0.0f;      // value set by the last applied event
    float m_current_y = 0.0f;
    uint8_t m_current_active = 0;
    bool m_ramping = false;  // x / y ramp towards the oldest waiting event
    float m_ramp_from_x = 0.0f;
    float m_ramp_from_y = 0.0f;
    int64_t m_ramp_from_sample = 0;
    uint32_t m_num_samples = 0;
    uint32_t m_num_change_points = 0;
    std::vector<float> m_x;
    std::vector<float> m_y;
    std::vector<uint8_t> m_active;
    std::vector<uint32_t> m_change_points;
};

}  // namespace thl::modulation::detail
