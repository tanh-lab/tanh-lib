#include "tanh/modulation/detail/MotionTakes.h"

#include <tanh/dsp/transport/TransportInfo.h>
#include <tanh/modulation/MotionLane.h>
#include <tanh/modulation/MotionLaneProcessing.h>
#include <tanh/modulation/MotionTypes.h>
#include <tanh/modulation/detail/MotionSeam.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace thl::modulation::detail {

using thl::dsp::transport::TransportInfo;

namespace {

constexpr size_t k_max_aligned_bars = 16;
constexpr size_t k_min_capacity = 16;
constexpr double k_tick_eps = 1e-9;
// A snapped free take counts as still while x and y stay within this distance
// (pad units, |dx| + |dy|) of its first or last point: finger jitter, no move.
constexpr double k_still_distance = 0.002;

// Nearest of 1, 2, 4, 8, 16 bars by ratio (log2), so the stretch is the smallest.
double snap_bars(double bars) {
    if (!(bars > 0.0)) { return 1.0; }
    const double exponent = std::clamp(std::round(std::log2(bars)), 0.0, 4.0);
    return std::exp2(exponent);
}

}  // namespace

void MotionTakeWriter::prepare(const MotionRecorderConfig& config, double sample_rate) {
    m_take = Take{};
    m_config = config;
    m_config.m_max_points = std::max(m_config.m_max_points, k_min_capacity);
    if (sample_rate > 0.0) { m_sample_rate = sample_rate; }
    for (auto& b : m_buffers) {
        b.m_state.store(TakeBufferState::Free, std::memory_order_relaxed);
        if (b.m_x.size() != m_config.m_max_points) {
            b.m_x.assign(m_config.m_max_points, 0.0f);
            b.m_y.assign(m_config.m_max_points, 0.0f);
            b.m_gate.assign(m_config.m_max_points, 0);
            // The seam window is at most a quarter of the take.
            b.m_raw_tail_x.assign(m_config.m_max_points / 4, 0.0f);
            b.m_raw_tail_y.assign(m_config.m_max_points / 4, 0.0f);
        }
    }
}

size_t MotionTakeWriter::buffer_bytes() const {
    size_t bytes = 0;
    for (const auto& b : m_buffers) {
        bytes += (b.m_x.capacity() + b.m_y.capacity() + b.m_raw_tail_x.capacity() +
                  b.m_raw_tail_y.capacity()) *
                     sizeof(float) +
                 b.m_gate.capacity();
    }
    return bytes;
}

float MotionTakeWriter::progress() const {
    if (!m_take.m_active || m_take.m_target == 0) { return 0.0f; }
    return static_cast<float>(
        std::clamp(static_cast<double>(m_take.m_count) / static_cast<double>(m_take.m_target),
                   0.0,
                   1.0));
}

MotionTakeWriter::Start MotionTakeWriter::start(const TransportInfo& t,
                                                LoopLength length,
                                                float first_x,
                                                float first_y,
                                                double clock_beat) TANH_NONBLOCKING_FUNCTION {
    uint32_t buffer = 2;
    for (uint32_t b = 0; b < 2; ++b) {
        if (m_buffers[b].m_state.load(std::memory_order_acquire) == TakeBufferState::Free) {
            buffer = b;
            break;
        }
    }
    if (buffer == 2) { return Start::Busy; }

    const double bpm = t.m_bpm > 0.0 ? t.m_bpm : 120.0;
    const double ideal_tpb = std::clamp(std::round(m_config.m_rate * 60.0 / bpm),
                                        static_cast<double>(m_config.m_min_tpb),
                                        static_cast<double>(m_config.m_max_tpb));
    const uint32_t bars = loop_length_bars(length);
    const auto max_points = static_cast<double>(m_config.m_max_points);
    const int sig_num = t.m_sig_num > 0 ? t.m_sig_num : 4;
    const int sig_denom = t.m_sig_denom > 0 ? t.m_sig_denom : 4;
    Take& k = m_take;
    k = Take{};
    k.m_bar_beats = static_cast<double>(sig_num) * 4.0 / static_cast<double>(sig_denom);

    if (m_config.m_bar_aligned_takes) {
        // Beats on the transport or the free-running clock. A free take reserves
        // 16 bars, so a whole number of points per bar has to fit 16 times.
        const size_t loop_bars = bars > 0 ? bars : k_max_aligned_bars;
        const double per_bar = std::min(std::round(k.m_bar_beats * ideal_tpb),
                                        std::floor(max_points / static_cast<double>(loop_bars)));
        if (per_bar < (k.m_bar_beats * m_config.m_min_tpb) - k_tick_eps) { return Start::Refused; }
        k.m_aligned = bars > 0;
        k.m_snap = bars == 0;
        k.m_bar = bars > 0;
        k.m_beats = true;
        k.m_points_per_bar = static_cast<size_t>(per_bar);
        k.m_capacity = loop_bars * k.m_points_per_bar;
        k.m_target = bars > 0 ? k.m_capacity : k.m_points_per_bar;
        k.m_length = static_cast<double>(loop_bars) * k.m_bar_beats;
        k.m_tick_step = k.m_bar_beats / per_bar;
        // A bar take's point 0 sits on the bar line at or before the touch; a
        // free take's on the first touch (record() rebases it there).
        k.m_bar_line = std::floor((clock_beat / k.m_bar_beats) + k_tick_eps) * k.m_bar_beats;
        k.m_tick_base = bars > 0 ? k.m_bar_line : clock_beat;
        k.m_clock = clock_beat;
    } else if (bars > 0) {
        const double loop =
            bars * static_cast<double>(sig_num) * 4.0 / static_cast<double>(sig_denom);
        const double points = std::min(std::round(loop * ideal_tpb), max_points);
        if (points < (loop * m_config.m_min_tpb) - k_tick_eps) { return Start::Refused; }
        k.m_bar = true;
        k.m_beats = true;
        k.m_length = loop;
        k.m_capacity = static_cast<size_t>(points);
        k.m_tick_step = loop / points;
        const double j0 = std::ceil((clock_beat / k.m_tick_step) - k_tick_eps);
        k.m_tick_base = j0 * k.m_tick_step;
        const double wrapped = j0 - (std::floor(j0 / points) * points);
        k.m_start_index = std::min(static_cast<size_t>(wrapped), k.m_capacity - 1);
        k.m_clock = clock_beat;
    } else if (t.is_playing()) {
        k.m_beats = true;
        k.m_capacity = m_config.m_max_points;
        k.m_tick_step = 1.0 / ideal_tpb;
        k.m_tick_base = clock_beat;
        k.m_clock = clock_beat;
    } else {
        k.m_capacity = m_config.m_max_points;
        k.m_tick_step = m_sample_rate / m_config.m_rate;
        k.m_tick_base = 0.0;
        k.m_clock = 0.0;
    }
    if (!k.m_aligned && !k.m_snap) { k.m_target = k.m_capacity; }
    k.m_active = true;
    k.m_buffer = buffer;
    k.m_start_beat = clock_beat;
    k.m_last_x = first_x;
    k.m_last_y = first_y;
    m_buffers[buffer].m_state.store(TakeBufferState::Recording, std::memory_order_relaxed);
    if (k.m_aligned) {
        // The time from the bar line to the touch is lifted (gate 0) at the
        // first position. At most one bar of points: a bounded loop.
        const double ticks = std::ceil(((clock_beat - k.m_bar_line) / k.m_tick_step) - k_tick_eps);
        const auto lead = std::min(static_cast<size_t>(std::max(ticks, 0.0)), k.m_target - 1);
        for (size_t i = 0; i < lead; ++i) { write_point(first_x, first_y, 0); }
    }
    return Start::Started;
}

MotionTakeWriter::Outcome MotionTakeWriter::step(bool live,
                                                 float x,
                                                 float y,
                                                 double rec_slope,
                                                 double bpm,
                                                 uint32_t block_samples) TANH_NONBLOCKING_FUNCTION {
    Take& k = m_take;
    if (!k.m_active) { return Outcome::Running; }
    if (!k.m_bar && k.m_saw_touch && !live) {
        return finish(bpm, block_samples);  // free take: ends on release
    }
    if (k.m_snap && !k.m_saw_touch && !live) {
        // record(Free) without a finger: the take starts at the touch.
        k.m_clock += rec_slope;
        return Outcome::Running;
    }
    if (k.m_snap && !k.m_saw_touch) {
        k.m_tick_base = k.m_clock;
        k.m_start_beat = k.m_clock;
    }
    k.m_saw_touch = k.m_saw_touch || live;
    Outcome outcome = Outcome::Running;
    const double next = k.m_tick_base + (static_cast<double>(k.m_count) * k.m_tick_step);
    if (k.m_clock >= next - k_tick_eps) {
        if (live) {
            write_point(x, y, 1);
        } else {
            write_point(k.m_last_x, k.m_last_y, 0);
        }
        // A snapped free take ends after 16 bars with the finger down; its
        // progress runs to the next 1, 2, 4, 8 or 16 bars.
        if (k.m_count >= k.m_target) {
            if (k.m_count >= k.m_capacity) {
                outcome = finish(bpm);
            } else {
                k.m_target = std::min(k.m_target * 2, k.m_capacity);
            }
        }
    }
    k.m_clock += k.m_beats ? rec_slope : 1.0;
    return outcome;
}

void MotionTakeWriter::write_point(float x, float y, uint8_t gate) TANH_NONBLOCKING_FUNCTION {
    Take& k = m_take;
    TakeBuffer& b = m_buffers[k.m_buffer];
    const size_t l = k.m_count;
    // Raw points: smoothing is a playback setting.
    size_t p = k.m_start_index + l;
    if (p >= k.m_capacity) { p -= k.m_capacity; }
    b.m_x[p] = x;
    b.m_y[p] = y;
    b.m_gate[p] = gate;
    if (l == 0 || x != k.m_last_x || y != k.m_last_y) { k.m_count_at_change = l + 1; }
    k.m_count = l + 1;
    k.m_last_x = x;
    k.m_last_y = y;
}

// A snapped free take loops without a pause: drop the points before the finger
// first moves and after it last moved (each run kept down to one point). The
// shift is a bounded memmove inside the take buffer.
void MotionTakeWriter::trim_still_ends() TANH_NONBLOCKING_FUNCTION {
    Take& k = m_take;
    TakeBuffer& b = m_buffers[k.m_buffer];
    const size_t n = k.m_count;
    auto distance = [&](size_t i, size_t j) {
        return std::abs(static_cast<double>(b.m_x[i]) - b.m_x[j]) +
               std::abs(static_cast<double>(b.m_y[i]) - b.m_y[j]);
    };
    size_t first = 0;
    while (first < n && distance(first, 0) <= k_still_distance) { ++first; }
    if (first == n) { return; }  // never moved: a held position, kept as it is
    size_t last = n - 1;
    while (last > 0 && distance(last, n - 1) <= k_still_distance) { --last; }
    const size_t begin = first - 1;
    const size_t end = std::min(n, std::max(last + 2, first + 1));
    if (begin > 0) {
        const auto from = static_cast<std::ptrdiff_t>(begin);
        const auto to = static_cast<std::ptrdiff_t>(end);
        std::copy(b.m_x.begin() + from, b.m_x.begin() + to, b.m_x.begin());
        std::copy(b.m_y.begin() + from, b.m_y.begin() + to, b.m_y.begin());
        std::copy(b.m_gate.begin() + from, b.m_gate.begin() + to, b.m_gate.begin());
    }
    k.m_start_beat += static_cast<double>(begin) * k.m_tick_step;
    k.m_count = end - begin;
}

// LoopEnd::Smooth: close the seam in place so the buffer plays without a jump at
// the wrap until service() publishes it (a finger held across the loop end). The
// raw values of the blended points go to the raw tail: service() restores them,
// so the published lane keeps the raw seam. Bounded work on preallocated memory.
void MotionTakeWriter::close_seam(TakeBuffer& b, size_t n) TANH_NONBLOCKING_FUNCTION {
    const size_t start = m_take.m_start_index;
    b.m_seam = start;
    b.m_seam_window = 0;
    if (n < k_motion_min_points || m_loop_end == LoopEnd::Jump) { return; }
    const size_t window = motion_seam_window(n, b.m_length, b.m_timebase);
    assert(window <= b.m_raw_tail_x.size());
    auto close = [&](std::vector<float>& p, std::vector<float>& tail) {
        auto at = [&](size_t i) -> float& {
            const size_t q = start + i;
            return p[q >= n ? q - n : q];
        };
        for (size_t j = 0; j < window; ++j) { tail[j] = at(n - window + j); }
        motion_spread_seam(at, n, window, motion_seam_error(at, n));
    };
    close(b.m_x, b.m_raw_tail_x);
    close(b.m_y, b.m_raw_tail_y);
    b.m_seam_window = window;
}

MotionTakeWriter::Outcome MotionTakeWriter::drop(Outcome outcome) TANH_NONBLOCKING_FUNCTION {
    m_buffers[m_take.m_buffer].m_state.store(TakeBufferState::Free, std::memory_order_relaxed);
    m_take.m_active = false;
    return outcome;
}

void MotionTakeWriter::abort() TANH_NONBLOCKING_FUNCTION {
    if (m_take.m_active) { (void)drop(Outcome::Cancelled); }
}

MotionTakeWriter::Outcome MotionTakeWriter::finish(double bpm, uint32_t trim_samples)
    TANH_NONBLOCKING_FUNCTION {
    Take& k = m_take;
    if (!k.m_active) { return Outcome::Running; }
    if (!k.m_bar && !k.m_snap && trim_samples > 0 && k.m_count_at_change >= k_motion_min_points) {
        // The release reaches us up to a block after the last move (events carry
        // no timestamps): a still tail no longer than that block is quantisation,
        // not gesture, and would put a pause into the loop.
        const double samples_per_point =
            k.m_beats ? k.m_tick_step * 60.0 * m_sample_rate / bpm : k.m_tick_step;
        const auto tail = static_cast<double>(k.m_count - k.m_count_at_change);
        if (tail * samples_per_point <= static_cast<double>(trim_samples)) {
            k.m_count = k.m_count_at_change;
        }
    }
    if (k.m_snap) { trim_still_ends(); }
    const bool complete = k.m_aligned ? k.m_count == k.m_target
                          : k.m_bar   ? k.m_count == k.m_capacity
                                      : k.m_count >= k_motion_min_points;
    if (!complete) { return drop(Outcome::Dropped); }

    TakeBuffer& b = m_buffers[k.m_buffer];
    const size_t n = k.m_count;
    if (k.m_bar && !k.m_aligned) {
        b.m_timebase = MotionTimebase::Beats;
        b.m_length = k.m_length;
        b.m_anchor = 0.0;
        b.m_rate = static_cast<double>(n) / k.m_length;
    } else if (k.m_aligned) {
        // Whole bars from the bar line: index 0 plays on that bar line.
        const double length =
            static_cast<double>(n) / static_cast<double>(k.m_points_per_bar) * k.m_bar_beats;
        b.m_timebase = MotionTimebase::Beats;
        b.m_length = length;
        b.m_anchor = wrap_phase(k.m_bar_line, length);
        b.m_rate = static_cast<double>(n) / length;
    } else if (k.m_snap) {
        // Stretched onto the nearest of 1, 2, 4, 8 or 16 bars; index 0 plays on
        // the bar line nearest the first kept point.
        const double beats = static_cast<double>(n) * k.m_tick_step;
        const double length = snap_bars(beats / k.m_bar_beats) * k.m_bar_beats;
        b.m_timebase = MotionTimebase::Beats;
        b.m_length = length;
        b.m_anchor = wrap_phase(std::round(k.m_start_beat / k.m_bar_beats) * k.m_bar_beats, length);
        b.m_rate = static_cast<double>(n) / length;
    } else if (k.m_beats) {
        // Free take while the transport plays: round to whole beats, stretch at read time.
        const double elapsed = static_cast<double>(n) * k.m_tick_step;
        const double length = std::max(1.0, std::round(elapsed));
        b.m_timebase = MotionTimebase::Beats;
        b.m_length = length;
        b.m_anchor = wrap_phase(std::round(k.m_start_beat * 16.0) / 16.0, length);
        b.m_rate = 1.0 / k.m_tick_step;
    } else {
        b.m_timebase = MotionTimebase::Seconds;
        b.m_rate = m_sample_rate / k.m_tick_step;
        b.m_length = static_cast<double>(n) / b.m_rate;
        b.m_anchor = 0.0;
    }
    close_seam(b, n);
    b.m_num_points = n;
    b.m_take_id = m_next_take_id.fetch_add(1, std::memory_order_relaxed);
    b.m_state.store(TakeBufferState::Finished, std::memory_order_release);
    k.m_active = false;
    m_finished_buffer = k.m_buffer;
    return Outcome::Finished;
}

// disarm() / stop() never throw a touched bar-aligned take away: a BarsN take
// ends at once with the rest of its N bars lifted, a free one where it is.
// Padding writes at most m_max_points points into preallocated memory.
MotionTakeWriter::Outcome MotionTakeWriter::end(double bpm) TANH_NONBLOCKING_FUNCTION {
    const Take& k = m_take;
    if (!k.m_active) { return Outcome::Running; }
    if (k.m_snap) { return k.m_saw_touch ? finish(bpm) : drop(Outcome::Cancelled); }
    if (!k.m_aligned) { return k.m_bar ? drop(Outcome::Dropped) : finish(bpm); }
    if (!k.m_saw_touch) { return drop(Outcome::Cancelled); }
    while (k.m_count < k.m_target) { write_point(k.m_last_x, k.m_last_y, 0); }
    return finish(bpm);
}

}  // namespace thl::modulation::detail
