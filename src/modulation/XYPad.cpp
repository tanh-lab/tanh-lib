#include "tanh/modulation/detail/XYPad.h"

#include <tanh/modulation/MotionRecorder.h>
#include <tanh/modulation/XYController.h>
#include <tanh/modulation/detail/EventSpread.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace thl::modulation::detail {

XYPad::XYPad(uint32_t max_touches, MonoPriority priority)
    : m_max_touches(std::clamp<uint32_t>(max_touches, 1, k_xy_controller_max_touches))
    , m_priority(priority) {}

void XYPad::prepare(size_t max_block_size) {
    if (max_block_size <= m_x.size()) { return; }
    m_x.assign(max_block_size, m_last_x);
    m_y.assign(max_block_size, m_last_y);
    m_active.assign(max_block_size, m_last_active);
    m_change_points.assign(max_block_size, 0);
    m_num_samples = 0;
    m_num_change_points = 0;
}

uint32_t XYPad::find_slot(TouchId id) const {
    for (uint32_t s = 0; s < m_max_touches; ++s) {
        if (m_slots[s].m_held && m_slots[s].m_id == id) { return s; }
    }
    return k_no_slot;
}

uint32_t XYPad::pick_driver() const {
    uint32_t best = k_no_slot;
    for (uint32_t s = 0; s < m_max_touches; ++s) {
        if (!m_slots[s].m_held) { continue; }
        if (best == k_no_slot) {
            best = s;
            continue;
        }
        const bool newer = m_slots[s].m_press_order > m_slots[best].m_press_order;
        if (m_priority == MonoPriority::Last ? newer : !newer) { best = s; }
    }
    return best;
}

bool XYPad::flush() {
    Pending& p = m_pending;
    if (p.m_position) {
        const auto kind = p.m_position_down ? XYPadEventKind::Down : XYPadEventKind::Move;
        if (!m_queue.try_push({.m_kind = kind, .m_x = p.m_position_x, .m_y = p.m_position_y})) {
            return false;
        }
        p.m_position = false;
        p.m_position_down = false;
    }
    if (p.m_up) {
        if (!m_queue.try_push({.m_kind = XYPadEventKind::Up, .m_x = p.m_up_x, .m_y = p.m_up_y})) {
            return false;
        }
        p.m_up = false;
    }
    if (p.m_down_after_up) {
        if (!m_queue.try_push(
                {.m_kind = XYPadEventKind::Down, .m_x = p.m_down_x, .m_y = p.m_down_y})) {
            return false;
        }
        p.m_down_after_up = false;
    }
    return true;
}

bool XYPad::send_position(float x, float y, bool down) {
    Pending& p = m_pending;
    if (p.any() && !flush()) {
        // Still blocked: coalesce behind what is pending (the latest position wins).
        if (p.m_up) {
            p.m_down_after_up = true;
            p.m_down_x = x;
            p.m_down_y = y;
        } else {
            p.m_position = true;
            p.m_position_down = p.m_position_down || down;
            p.m_position_x = x;
            p.m_position_y = y;
        }
        return true;
    }
    const auto kind = down ? XYPadEventKind::Down : XYPadEventKind::Move;
    if (m_queue.try_push({.m_kind = kind, .m_x = x, .m_y = y})) { return true; }
    if (down) { return false; }  // a new touch that cannot be sent claims nothing
    p.m_position = true;
    p.m_position_x = x;
    p.m_position_y = y;
    return true;
}

void XYPad::send_up(float x, float y) {
    Pending& p = m_pending;
    if (!(p.any() && !flush()) &&
        m_queue.try_push({.m_kind = XYPadEventKind::Up, .m_x = x, .m_y = y})) {
        return;
    }
    // A down still pending behind the up is a tap that never reached the audio
    // thread: drop it, and let the pending up carry the final position.
    p.m_down_after_up = false;
    p.m_up = true;
    p.m_up_x = x;
    p.m_up_y = y;
}

bool XYPad::touch(TouchId id, float x, float y) {
    if (!std::isfinite(x) || !std::isfinite(y)) { return false; }
    x = std::clamp(x, 0.0f, 1.0f);
    y = std::clamp(y, 0.0f, 1.0f);
    flush();

    uint32_t slot = find_slot(id);
    if (slot != k_no_slot) {
        m_slots[slot].m_x = x;
        m_slots[slot].m_y = y;
        return slot != m_driver || send_position(x, y, false);
    }

    for (slot = 0; slot < m_max_touches; ++slot) {
        if (!m_slots[slot].m_held) { break; }
    }
    if (slot == m_max_touches) { return false; }

    // Under Last priority a new finger takes over a held stream with a move, so
    // the gate stays open.
    const bool idle = m_driver == k_no_slot;
    if (idle || m_priority == MonoPriority::Last) {
        if (!send_position(x, y, idle)) { return false; }
        m_driver = slot;
    }
    m_slots[slot] =
        Slot{.m_id = id, .m_x = x, .m_y = y, .m_held = true, .m_press_order = ++m_press_counter};
    return true;
}

bool XYPad::release(TouchId id) {
    flush();
    const uint32_t slot = find_slot(id);
    if (slot == k_no_slot) { return false; }
    Slot& released = m_slots[slot];
    released.m_held = false;
    if (slot != m_driver) { return true; }

    // The driver left: fall back to a remaining finger, or close the gate.
    m_driver = pick_driver();
    if (m_driver != k_no_slot) {
        send_position(m_slots[m_driver].m_x, m_slots[m_driver].m_y, false);
    } else {
        send_up(released.m_x, released.m_y);
    }
    return true;
}

void XYPad::release_all() {
    flush();
    for (auto& s : m_slots) { s.m_held = false; }
    if (m_driver != k_no_slot) {
        const Slot& driver = m_slots[m_driver];
        m_driver = k_no_slot;
        send_up(driver.m_x, driver.m_y);
    }
}

void XYPad::apply(const XYPadEvent& event, uint32_t offset, uint32_t num_samples) {
    std::fill(m_x.begin() + offset, m_x.begin() + num_samples, event.m_x);
    std::fill(m_y.begin() + offset, m_y.begin() + num_samples, event.m_y);
    const uint8_t gate = event.m_kind == XYPadEventKind::Up ? 0 : 1;
    std::fill(m_active.begin() + offset, m_active.begin() + num_samples, gate);
    if (m_num_change_points == 0 || m_change_points[m_num_change_points - 1] != offset) {
        m_change_points[m_num_change_points++] = offset;
    }
}

void XYPad::process_block(uint32_t num_samples) TANH_NONBLOCKING_FUNCTION {
    const auto n = static_cast<uint32_t>(std::min<size_t>(num_samples, m_x.size()));
    m_num_samples = n;
    m_num_change_points = 0;
    if (n == 0) { return; }  // keep the events for a real block

    std::fill_n(m_x.begin(), n, m_last_x);
    std::fill_n(m_y.begin(), n, m_last_y);
    std::fill_n(m_active.begin(), n, m_last_active);

    // Events carry no timestamps: spread the ones drained this block evenly.
    size_t count = 0;
    while (count < m_scratch.size() && m_queue.try_pop(m_scratch[count])) { ++count; }
    for (size_t i = 0; i < count; ++i) { apply(m_scratch[i], spread_offset(i, count, n), n); }

    m_last_x = m_x[n - 1];
    m_last_y = m_y[n - 1];
    m_last_active = m_active[n - 1];
}

MotionInput XYPad::output() const TANH_NONBLOCKING_FUNCTION {
    if (m_x.empty()) { return {}; }
    return MotionInput{
        .m_x = m_x.data(),
        .m_y = m_y.data(),
        .m_active = m_active.data(),
        .m_change_points = std::span<const uint32_t>(m_change_points.data(), m_num_change_points),
        .m_num_samples = m_num_samples,
    };
}

}  // namespace thl::modulation::detail
