#include "tanh/modulation/detail/MotionLaneStore.h"

#include <tanh/modulation/MotionLane.h>
#include <tanh/modulation/MotionLaneProcessing.h>
#include <tanh/modulation/detail/MotionTakeRecorder.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "MotionInterpolation.h"
#include "MotionSeam.h"

namespace thl::modulation::detail {

MotionLaneStore::MotionLaneStore() {
    m_audio_reader = m_lanes.add_reader();
}

void MotionLaneStore::prepare(size_t max_points) {
    max_points = std::max<size_t>(max_points, k_motion_min_points);
    for (auto& s : m_slots) {
        s.m_state.store(SlotState::Free, std::memory_order_relaxed);
        if (s.m_x.size() != max_points) {
            s.m_x.assign(max_points, 0.0f);
            s.m_y.assign(max_points, 0.0f);
            s.m_gate.assign(max_points, 0);
            // The seam window is at most a quarter of the loop.
            s.m_raw_tail_x.assign(max_points / 4, 0.0f);
            s.m_raw_tail_y.assign(max_points / 4, 0.0f);
        }
    }
}

bool MotionLaneStore::submit(const MotionLoopView& take,
                             uint32_t serial) TANH_NONBLOCKING_FUNCTION {
    Slot* slot = nullptr;
    for (auto& s : m_slots) {
        if (s.m_state.load(std::memory_order_acquire) == SlotState::Free) {
            slot = &s;
            break;
        }
    }
    const size_t n = std::min(take.m_num_points, slot != nullptr ? slot->m_x.size() : 0);
    if (slot == nullptr || n == 0) { return false; }
    std::copy_n(take.m_x, n, slot->m_x.begin());
    std::copy_n(take.m_y, n, slot->m_y.begin());
    if (take.m_gate != nullptr) {
        std::copy_n(take.m_gate, n, slot->m_gate.begin());
    } else {
        std::fill_n(slot->m_gate.begin(), n, uint8_t{1});
    }
    slot->m_view = take;
    slot->m_view.m_num_points = n;
    slot->m_view.m_seam = take.m_seam < n ? take.m_seam : 0;
    slot->m_view.m_x = slot->m_x.data();
    slot->m_view.m_y = slot->m_y.data();
    slot->m_view.m_gate = slot->m_gate.data();
    slot->m_seam_window = 0;

    // LoopEnd::Smooth: close the seam in place so the slot plays without a jump until service()
    // publishes it; the raw values of the moved points go to the raw tail.
    if (n >= k_motion_min_points && m_loop_end.load(std::memory_order_relaxed) == LoopEnd::Smooth) {
        const size_t seam = slot->m_view.m_seam;
        const size_t window = motion_seam_window(n, take.m_length, take.m_timebase);
        assert(window <= slot->m_raw_tail_x.size());
        auto close = [&](std::vector<float>& p, std::vector<float>& tail) {
            auto at = [&](size_t i) -> float& {
                const size_t q = seam + i;
                return p[q >= n ? q - n : q];
            };
            for (size_t j = 0; j < window; ++j) { tail[j] = at(n - window + j); }
            motion_spread_seam(at, n, window, motion_seam_error(at, n));
        };
        close(slot->m_x, slot->m_raw_tail_x);
        close(slot->m_y, slot->m_raw_tail_y);
        slot->m_seam_window = window;
    }
    slot->m_id = m_next_id.fetch_add(1, std::memory_order_relaxed);
    slot->m_serial = serial;
    slot->m_state.store(SlotState::Ready, std::memory_order_release);
    return true;
}

MotionLaneSource MotionLaneStore::slot_source(const Slot& slot) const TANH_NONBLOCKING_FUNCTION {
    return {.m_view = slot.m_view, .m_seed = slot.m_view, .m_id = slot.m_id, .m_version = 0};
}

// A slot turns Published after service() made its lane, possibly after this block's read
// scope opened: whether it still plays depends only on the lane this block reads.
MotionLaneSource MotionLaneStore::select(const MotionPlayedLane& published)
    TANH_NONBLOCKING_FUNCTION {
    const uint32_t lane_id = published.m_lane.m_take_id;
    const Slot* newest = nullptr;
    for (auto& s : m_slots) {
        const SlotState state = s.m_state.load(std::memory_order_acquire);
        if (state == SlotState::Free) { continue; }
        if (s.m_id <= lane_id || dropped(s.m_serial)) {
            if (state == SlotState::Published) {
                s.m_state.store(SlotState::Free, std::memory_order_relaxed);
            }
            continue;
        }
        if (newest == nullptr || s.m_id > newest->m_id) { newest = &s; }
    }
    if (newest != nullptr) { return slot_source(*newest); }

    const MotionLane& lane = published.m_lane;
    MotionLaneSource source;
    const size_t n = std::min({lane.m_x.size(),
                               lane.m_y.size(),
                               lane.m_gate.size(),
                               published.m_x.size(),
                               published.m_y.size()});
    source.m_id = lane_id;
    source.m_version = published.m_version;
    source.m_smoothed = published.m_smoothed;
    source.m_view = {.m_x = published.m_x.data(),
                     .m_y = published.m_y.data(),
                     .m_gate = lane.m_gate.data(),
                     .m_num_points = n,
                     .m_timebase = lane.m_timebase,
                     .m_length = lane.m_length,
                     .m_anchor = lane.m_anchor,
                     .m_seam = lane.m_seam < n ? lane.m_seam : 0};
    source.m_seed = source.m_view;
    if (published.m_seed_x.size() >= n && published.m_seed_y.size() >= n &&
        !published.m_seed_x.empty()) {
        source.m_seed.m_x = published.m_seed_x.data();
        source.m_seed.m_y = published.m_seed_y.data();
    }
    return source;
}

bool MotionLaneStore::service() {
    std::array<Slot*, 2> ready{};
    size_t count = 0;
    for (auto& s : m_slots) {
        if (s.m_state.load(std::memory_order_acquire) == SlotState::Ready) { ready[count++] = &s; }
    }
    if (count == 2 && ready[1]->m_id < ready[0]->m_id) { std::swap(ready[0], ready[1]); }
    bool published = false;
    for (size_t k = 0; k < count; ++k) {
        Slot& s = *ready[k];
        // A lane loaded after the take is newer, and a cleared take keeps nothing: drop it.
        if (s.m_id > m_published_id && !dropped(s.m_serial)) {
            const MotionLoopView& v = s.m_view;
            const size_t n = v.m_num_points;
            MotionLane lane;
            lane.m_take_id = s.m_id;
            lane.m_timebase = v.m_timebase;
            lane.m_length = v.m_length;
            lane.m_anchor = v.m_anchor;
            lane.m_rate = static_cast<double>(n) / v.m_length;
            lane.m_seam = v.m_seam;
            const auto end = static_cast<std::ptrdiff_t>(n);
            lane.m_x.assign(s.m_x.begin(), s.m_x.begin() + end);
            lane.m_y.assign(s.m_y.begin(), s.m_y.begin() + end);
            lane.m_gate.assign(s.m_gate.begin(), s.m_gate.begin() + end);
            // The lane keeps the raw recording; what plays closes the seam again.
            for (size_t j = 0; j < s.m_seam_window; ++j) {
                const size_t i = (v.m_seam + n - s.m_seam_window + j) % n;
                lane.m_x[i] = s.m_raw_tail_x[j];
                lane.m_y[i] = s.m_raw_tail_y[j];
            }
            const bool first_revision = s.m_serial == 0 || s.m_serial != m_history_serial;
            m_history_serial = s.m_serial;
            publish(std::move(lane), first_revision);
            published = true;
        }
        s.m_state.store(SlotState::Published, std::memory_order_release);
    }
    return published;
}

void MotionLaneStore::publish_played(MotionLane lane) {
    const uint32_t id = lane.m_take_id;
    const float smoothing = m_smoothing.load(std::memory_order_relaxed);
    const LoopEnd loop_end = m_loop_end.load(std::memory_order_relaxed);
    PlayedCurve curve = make_played_curve(lane, smoothing, loop_end);
    PlayedCurve seed;
    if (smoothing > 0.0f) { seed = make_played_curve(lane, 0.0f, loop_end); }
    const uint32_t version = ++m_played_version;
    m_lanes.replace([&](MotionPlayedLane& l) {
        l.m_x = std::move(curve.m_x);
        l.m_y = std::move(curve.m_y);
        l.m_seed_x = std::move(seed.m_x);
        l.m_seed_y = std::move(seed.m_y);
        l.m_smoothed = curve.m_smoothed;
        l.m_version = version;
        l.m_lane = std::move(lane);
    });
    m_published_id = id;
    m_version.fetch_add(1, std::memory_order_acq_rel);
}

void MotionLaneStore::publish(MotionLane lane, bool record_history) {
    if (record_history) { m_history.record(this->lane(), lane.empty()); }
    publish_played(std::move(lane));
}

uint32_t MotionLaneStore::load(MotionLane lane, size_t max_points) {
    const size_t n = lane.num_points();
    if (lane.m_y.size() != n || (n > 0 && lane.m_seam >= n)) { return 0; }
    if (lane.m_gate.size() != n) {
        if (!lane.m_gate.empty()) { return 0; }
        lane.m_gate.assign(n, 1);
    }
    if (n > 0) {
        if (!std::isfinite(lane.m_length) || !(lane.m_length > 0.0) ||
            !std::isfinite(lane.m_anchor) || !std::isfinite(lane.m_rate) || !(lane.m_rate > 0.0)) {
            return 0;
        }
        for (size_t i = 0; i < n; ++i) {
            if (!std::isfinite(lane.m_x[i]) || !std::isfinite(lane.m_y[i])) { return 0; }
            lane.m_x[i] = std::clamp(lane.m_x[i], 0.0f, 1.0f);
            lane.m_y[i] = std::clamp(lane.m_y[i], 0.0f, 1.0f);
            lane.m_gate[i] = lane.m_gate[i] != 0 ? 1 : 0;
        }
        lane.m_anchor = wrap_phase(lane.m_anchor, lane.m_length);
        if (n > max_points) { lane.resample(max_points); }
    }
    lane.m_take_id = m_next_id.fetch_add(1, std::memory_order_relaxed);
    const uint32_t id = lane.m_take_id;
    m_history_serial = 0;
    publish(std::move(lane), true);
    return id;
}

uint32_t MotionLaneStore::clear() {
    return load(MotionLane{}, 0);
}

bool MotionLaneStore::undo() {
    service();
    auto restored = m_history.undo(lane());
    if (!restored) { return false; }
    restored->m_take_id = m_next_id.fetch_add(1, std::memory_order_relaxed);
    m_history_serial = 0;
    publish(std::move(*restored), false);
    return true;
}

bool MotionLaneStore::redo() {
    service();
    auto restored = m_history.redo(lane());
    if (!restored) { return false; }
    restored->m_take_id = m_next_id.fetch_add(1, std::memory_order_relaxed);
    m_history_serial = 0;
    publish(std::move(*restored), false);
    return true;
}

void MotionLaneStore::set_smoothing(float amount) {
    const float a = std::isfinite(amount) ? std::clamp(amount, 0.0f, 1.0f) : 0.0f;
    if (a == m_smoothing.load(std::memory_order_relaxed)) { return; }
    m_smoothing.store(a, std::memory_order_relaxed);
    publish_played(lane());  // same take id: playback glides to the new curve
}

void MotionLaneStore::set_loop_end(LoopEnd mode) {
    if (mode == m_loop_end.load(std::memory_order_relaxed)) { return; }
    m_loop_end.store(mode, std::memory_order_relaxed);
    publish_played(lane());
}

MotionLane MotionLaneStore::lane() const {
    MotionLane copy;
    m_lanes.read([&](const MotionPlayedLane& l) { copy = l.m_lane; });
    return copy;
}

MotionLane MotionLaneStore::played_lane() const {
    MotionLane copy;
    m_lanes.read([&](const MotionPlayedLane& l) {
        copy = l.m_lane;
        copy.m_x = l.m_x;
        copy.m_y = l.m_y;
    });
    return copy;
}

}  // namespace thl::modulation::detail
