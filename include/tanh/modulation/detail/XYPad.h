#pragma once

#include <tanh/core/Exports.h>
#include <tanh/core/threading/LockFreeQueue.h>
#include <tanh/modulation/MotionRecorder.h>
#include <tanh/modulation/XYController.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace thl::modulation::detail {

/// Capacity of the UI to audio event queue of one pad.
inline constexpr size_t k_xy_pad_queue_capacity = 256;

enum class XYPadEventKind : uint8_t { Down, Move, Up };

/// One UI to audio pad event; x and y travel together.
struct XYPadEvent {
    XYPadEventKind m_kind = XYPadEventKind::Move;
    float m_x = 0.0f;
    float m_y = 0.0f;
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
 */
class TANH_API XYPad {
public:
    XYPad(uint32_t max_touches, MonoPriority priority);

    /// Allocate the stream for blocks of up to @p max_block_size samples.
    void prepare(size_t max_block_size);

    /// Touch down (new id) or move (known id). False for a non-finite position,
    /// a full touch table, or a new touch that cannot be queued.
    bool touch(TouchId id, float x, float y);
    /// Touch up or cancel. False for an unknown id.
    bool release(TouchId id);
    void release_all();
    /// Re-send coalesced events. True when nothing is pending.
    bool flush();

    /// Drain the queue and render @p num_samples (at most the prepared size).
    void process_block(uint32_t num_samples) TANH_NONBLOCKING_FUNCTION;
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

    // Events that could not be queued yet, in send order: position (down or
    // move), up, then a down that followed the up.
    struct Pending {
        bool m_position = false;
        bool m_position_down = false;
        float m_position_x = 0.0f;
        float m_position_y = 0.0f;
        bool m_up = false;
        float m_up_x = 0.0f;
        float m_up_y = 0.0f;
        bool m_down_after_up = false;
        float m_down_x = 0.0f;
        float m_down_y = 0.0f;
        [[nodiscard]] bool any() const { return m_position || m_up || m_down_after_up; }
    };

    bool send_position(float x, float y, bool down);
    void send_up(float x, float y);
    [[nodiscard]] uint32_t find_slot(TouchId id) const;
    [[nodiscard]] uint32_t pick_driver() const;
    void apply(const XYPadEvent& event, uint32_t offset, uint32_t num_samples);

    uint32_t m_max_touches = 1;
    MonoPriority m_priority = MonoPriority::Last;
    thl::core::LockFreeQueue<XYPadEvent, k_xy_pad_queue_capacity> m_queue;

    // UI thread only.
    std::array<Slot, k_xy_controller_max_touches> m_slots{};
    Pending m_pending;
    uint64_t m_press_counter = 0;
    uint32_t m_driver = k_no_slot;  // the slot that drives the stream

    // Audio thread only.
    std::array<XYPadEvent, k_xy_pad_queue_capacity> m_scratch{};
    float m_last_x = 0.0f;
    float m_last_y = 0.0f;
    uint8_t m_last_active = 0;
    uint32_t m_num_samples = 0;
    uint32_t m_num_change_points = 0;
    std::vector<float> m_x;
    std::vector<float> m_y;
    std::vector<uint8_t> m_active;
    std::vector<uint32_t> m_change_points;
};

}  // namespace thl::modulation::detail
