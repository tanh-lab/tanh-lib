#include "tanh/modulation/detail/XYPad.h"

#include <tanh/modulation/MotionRecorder.h>
#include <tanh/modulation/XYController.h>
#include <tanh/modulation/detail/EventSpread.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace thl::modulation::detail {

XYPad::XYPad(uint32_t max_touches, MonoPriority priority)
    : m_max_touches(std::clamp<uint32_t>(max_touches, 1, k_xy_controller_max_touches))
    , m_priority(priority) {}

void XYPad::prepare(size_t max_block_size, const XYPadTiming& timing) {
    m_timing = timing;
    m_timing.m_ramp_interval = std::max<uint32_t>(m_timing.m_ramp_interval, 1);
    // A timed event never waits longer than the delay plus one block: anything
    // later comes from a clock that does not match the block time.
    const double delay_samples =
        static_cast<double>(m_timing.m_delay_ns) * m_timing.m_sample_rate * 1e-9;
    m_max_wait = static_cast<int64_t>(std::ceil(std::max(delay_samples, 0.0))) +
                 static_cast<int64_t>(std::max(max_block_size, m_x.size()));
    if (max_block_size <= m_x.size()) { return; }
    m_x.assign(max_block_size, m_current_x);
    m_y.assign(max_block_size, m_current_y);
    m_active.assign(max_block_size, m_current_active);
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
    for (auto* event : {&p.m_position, &p.m_up, &p.m_down_after_up}) {
        if (!event->has_value()) { continue; }
        if (!m_queue.try_push(**event)) { return false; }
        event->reset();
    }
    return true;
}

namespace {

// The kind of two coalesced position events: a down or a jump survives later moves.
XYPadEventKind coalesce(XYPadEventKind earlier, XYPadEventKind later) {
    if (earlier == XYPadEventKind::Down || later == XYPadEventKind::Down) {
        return XYPadEventKind::Down;
    }
    if (earlier == XYPadEventKind::Jump || later == XYPadEventKind::Jump) {
        return XYPadEventKind::Jump;
    }
    return XYPadEventKind::Move;
}

}  // namespace

bool XYPad::send_position(XYPadEventKind kind, float x, float y, int64_t time_ns) {
    Pending& p = m_pending;
    const XYPadEvent event{.m_kind = kind, .m_x = x, .m_y = y, .m_time_ns = time_ns};
    if (p.any() && !flush()) {
        // Still blocked: coalesce behind what is pending (the latest position wins).
        if (p.m_up) {
            p.m_down_after_up = XYPadEvent{.m_kind = XYPadEventKind::Down,
                                           .m_x = x,
                                           .m_y = y,
                                           .m_time_ns = time_ns};
        } else {
            const XYPadEventKind earlier = p.m_position ? p.m_position->m_kind : kind;
            p.m_position = event;
            p.m_position->m_kind = coalesce(earlier, kind);
        }
        return true;
    }
    if (m_queue.try_push(event)) { return true; }
    if (kind == XYPadEventKind::Down) {
        return false;
    }  // a new touch that cannot be sent claims nothing
    p.m_position = event;
    return true;
}

void XYPad::send_up(float x, float y, int64_t time_ns) {
    Pending& p = m_pending;
    const XYPadEvent event{.m_kind = XYPadEventKind::Up, .m_x = x, .m_y = y, .m_time_ns = time_ns};
    if (!(p.any() && !flush()) && m_queue.try_push(event)) { return; }
    // A down still pending behind the up is a tap that never reached the audio
    // thread: drop it, and let the pending up carry the final position.
    p.m_down_after_up.reset();
    p.m_up = event;
}

bool XYPad::touch(TouchId id, float x, float y, int64_t time_ns) {
    if (!std::isfinite(x) || !std::isfinite(y)) { return false; }
    x = std::clamp(x, 0.0f, 1.0f);
    y = std::clamp(y, 0.0f, 1.0f);
    flush();

    uint32_t slot = find_slot(id);
    if (slot != k_no_slot) {
        m_slots[slot].m_x = x;
        m_slots[slot].m_y = y;
        return slot != m_driver || send_position(XYPadEventKind::Move, x, y, time_ns);
    }

    for (slot = 0; slot < m_max_touches; ++slot) {
        if (!m_slots[slot].m_held) { break; }
    }
    if (slot == m_max_touches) { return false; }

    // Under Last priority a new finger takes over a held stream with a jump, so
    // the gate stays open.
    const bool idle = m_driver == k_no_slot;
    if (idle || m_priority == MonoPriority::Last) {
        const auto kind = idle ? XYPadEventKind::Down : XYPadEventKind::Jump;
        if (!send_position(kind, x, y, time_ns)) { return false; }
        m_driver = slot;
    }
    m_slots[slot] =
        Slot{.m_id = id, .m_x = x, .m_y = y, .m_held = true, .m_press_order = ++m_press_counter};
    return true;
}

bool XYPad::release(TouchId id, int64_t time_ns) {
    flush();
    const uint32_t slot = find_slot(id);
    if (slot == k_no_slot) { return false; }
    Slot& released = m_slots[slot];
    released.m_held = false;
    if (slot != m_driver) { return true; }

    // The driver left: fall back to a remaining finger, or close the gate.
    m_driver = pick_driver();
    if (m_driver != k_no_slot) {
        send_position(XYPadEventKind::Jump, m_slots[m_driver].m_x, m_slots[m_driver].m_y, time_ns);
    } else {
        send_up(released.m_x, released.m_y, time_ns);
    }
    return true;
}

void XYPad::release_all() {
    flush();
    for (auto& s : m_slots) { s.m_held = false; }
    if (m_driver != k_no_slot) {
        const Slot& driver = m_slots[m_driver];
        m_driver = k_no_slot;
        send_up(driver.m_x, driver.m_y, k_xy_pad_untimed);
    }
}

void XYPad::add_change_point(uint32_t offset) {
    if (m_num_change_points == 0 || m_change_points[m_num_change_points - 1] != offset) {
        m_change_points[m_num_change_points++] = offset;
    }
}

void XYPad::apply(const XYPadEvent& event, uint32_t offset) {
    m_current_x = event.m_x;
    m_current_y = event.m_y;
    m_current_active = event.m_kind == XYPadEventKind::Up ? 0 : 1;
    add_change_point(offset);
}

// Move the queued events into the waiting ring and give each a pad sample.
// Untimed events (or any event without a block clock) are spread over this
// block as before; timed ones land at their timestamp plus the delay, at the
// block start when already late. No event is placed before an earlier one.
void XYPad::drain(uint32_t num_samples, std::optional<int64_t> block_time_ns) {
    const bool clock = block_time_ns.has_value() && m_timing.m_sample_rate > 0.0;
    const size_t first = m_waiting_count;
    XYPadEvent event;
    while (m_waiting_count < m_waiting.size() && m_queue.try_pop(event)) {
        m_waiting[(m_waiting_head + m_waiting_count) % m_waiting.size()].m_event = event;
        ++m_waiting_count;
    }
    const size_t drained = m_waiting_count - first;
    for (size_t i = 0; i < drained; ++i) {
        Scheduled& s = m_waiting[(m_waiting_head + first + i) % m_waiting.size()];
        s.m_timed = clock && s.m_event.m_time_ns != k_xy_pad_untimed;
        int64_t offset = spread_offset(i, drained, num_samples);
        if (s.m_timed) {
            offset = std::clamp<int64_t>(timed_offset(s.m_event.m_time_ns,
                                                      *block_time_ns,
                                                      m_timing.m_delay_ns,
                                                      m_timing.m_sample_rate),
                                         0,
                                         m_max_wait);
        }
        s.m_sample = std::max(m_block_start + offset, m_last_scheduled);
        m_last_scheduled = s.m_sample;
    }
}

// While the gate is open, x / y ramp from the current value at @p from_sample
// to the oldest waiting event when that is a timed move. The ramp starts when
// the move is known: at the previous event, or at a block start if the move
// arrived later than the delay covers.
void XYPad::start_ramp(int64_t from_sample) {
    m_ramping = false;
    if (m_waiting_count == 0 || m_current_active == 0) { return; }
    const Scheduled& next = m_waiting[m_waiting_head];
    if (!next.m_timed || next.m_event.m_kind != XYPadEventKind::Move ||
        next.m_sample <= from_sample) {
        return;
    }
    m_ramping = true;
    m_ramp_from_x = m_current_x;
    m_ramp_from_y = m_current_y;
    m_ramp_from_sample = from_sample;
}

void XYPad::render(uint32_t from, uint32_t to) {
    if (from >= to) { return; }
    std::fill(m_active.begin() + from, m_active.begin() + to, m_current_active);
    if (!m_ramping) {
        std::fill(m_x.begin() + from, m_x.begin() + to, m_current_x);
        std::fill(m_y.begin() + from, m_y.begin() + to, m_current_y);
        return;
    }
    const Scheduled& target = m_waiting[m_waiting_head];
    const auto span = static_cast<double>(target.m_sample - m_ramp_from_sample);
    const double dx = target.m_event.m_x - m_ramp_from_x;
    const double dy = target.m_event.m_y - m_ramp_from_y;
    for (uint32_t i = from; i < to; ++i) {
        const double w = static_cast<double>(m_block_start + i - m_ramp_from_sample) / span;
        m_x[i] = static_cast<float>(m_ramp_from_x + (dx * w));
        m_y[i] = static_cast<float>(m_ramp_from_y + (dy * w));
    }
    // Matrix targets update at change points: mark the ramp often enough to follow it.
    for (uint32_t i = from; i < to; i += m_timing.m_ramp_interval) { add_change_point(i); }
}

void XYPad::process_block(uint32_t num_samples,
                          std::optional<int64_t> block_time_ns) TANH_NONBLOCKING_FUNCTION {
    const auto n = static_cast<uint32_t>(std::min<size_t>(num_samples, m_x.size()));
    m_num_samples = n;
    m_num_change_points = 0;
    if (n == 0) { return; }  // keep the events for a real block

    drain(n, block_time_ns);
    if (!m_ramping) { start_ramp(m_block_start - 1); }

    // Render up to each due event, apply it, and ramp on towards the next one.
    // Events due after this block stay waiting.
    const int64_t end = m_block_start + n;
    uint32_t position = 0;
    while (m_waiting_count > 0) {
        const Scheduled& next = m_waiting[m_waiting_head];
        if (next.m_sample >= end) { break; }
        const auto at = static_cast<uint32_t>(next.m_sample - m_block_start);
        render(position, at);
        apply(next.m_event, at);
        m_waiting_head = (m_waiting_head + 1) % m_waiting.size();
        --m_waiting_count;
        position = at;
        start_ramp(m_block_start + at);
    }
    render(position, n);
    m_block_start = end;
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
