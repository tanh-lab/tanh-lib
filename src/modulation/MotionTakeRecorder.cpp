#include "tanh/modulation/detail/MotionTakeRecorder.h"

#include <tanh/modulation/MotionLane.h>
#include <tanh/modulation/MotionTypes.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "MotionInterpolation.h"
#include "MotionSeam.h"

namespace thl::modulation::detail {

namespace {

constexpr size_t k_min_capacity = 16;
constexpr size_t k_free_bars = 16;
constexpr double k_tick_eps = 1e-9;
// A free take counts as still while x and y stay within this distance (pad units,
// |dx| + |dy|) of its first or last point: finger jitter, no move.
constexpr double k_still_distance = 0.002;

// Nearest of 1, 2, 4, 8, 16 bars by ratio (log2), so the stretch is the smallest.
double snap_bars(double bars) {
    if (!(bars > 0.0)) { return 1.0; }
    return std::exp2(std::clamp(std::round(std::log2(bars)), 0.0, 4.0));
}

}  // namespace

void MotionTakeRecorder::prepare(const MotionRecorderConfig& config) {
    m_config = config;
    m_config.m_max_points = std::max(m_config.m_max_points, k_min_capacity);
    m_config.m_min_tpb = std::max<uint32_t>(m_config.m_min_tpb, 1);
    m_config.m_max_tpb = std::max(m_config.m_max_tpb, m_config.m_min_tpb);
    if (!(m_config.m_rate > 0.0)) { m_config.m_rate = 200.0; }
    m_x.assign(m_config.m_max_points, 0.0f);
    m_y.assign(m_config.m_max_points, 0.0f);
    m_gate.assign(m_config.m_max_points, 0);
    m_out_x.assign(m_config.m_max_points, 0.0f);
    m_out_y.assign(m_config.m_max_points, 0.0f);
    m_out_gate.assign(m_config.m_max_points, 0);
    m_active = false;
    m_num_points = 0;
}

size_t MotionTakeRecorder::buffer_bytes() const {
    return ((m_x.capacity() + m_y.capacity() + m_out_x.capacity() + m_out_y.capacity()) *
            sizeof(float)) +
           m_gate.capacity() + m_out_gate.capacity();
}

float MotionTakeRecorder::progress() const {
    if (!m_active) { return 0.0f; }
    const size_t of = m_bars ? m_capacity : m_target;
    if (of == 0) { return 0.0f; }
    return static_cast<float>(
        std::clamp(static_cast<double>(m_count) / static_cast<double>(of), 0.0, 1.0));
}

MotionLoopView MotionTakeRecorder::take() const {
    return {.m_x = m_bars ? m_out_x.data() : m_x.data(),
            .m_y = m_bars ? m_out_y.data() : m_y.data(),
            .m_gate = m_bars ? m_out_gate.data() : m_gate.data(),
            .m_num_points = m_num_points,
            .m_timebase = MotionTimebase::Beats,
            .m_length = m_length,
            .m_anchor = m_anchor,
            .m_seam = m_seam};
}

MotionTakeRecorder::Start MotionTakeRecorder::start(const MotionTakeStart& s)
    TANH_NONBLOCKING_FUNCTION {
    m_active = false;
    const double bpm = s.m_bpm > 0.0 ? s.m_bpm : 120.0;
    const int num = s.m_sig_num > 0 ? s.m_sig_num : 4;
    const int denom = s.m_sig_denom > 0 ? s.m_sig_denom : 4;
    m_bar_beats = static_cast<double>(num) * 4.0 / static_cast<double>(denom);
    const double bars = loop_length_bars(s.m_length);
    const double loop_bars = bars > 0.0 ? bars : static_cast<double>(k_free_bars);
    const double tpb = std::clamp(std::round(m_config.m_rate * 60.0 / bpm),
                                  static_cast<double>(m_config.m_min_tpb),
                                  static_cast<double>(m_config.m_max_tpb));
    // A whole number of points per bar that fits the loop (16 bars for a free take), and per
    // loop: a fraction of a bar rounds to whole points.
    const double per_bar =
        std::min(std::round(m_bar_beats * tpb),
                 std::floor(static_cast<double>(m_config.m_max_points) / loop_bars));
    const double points = std::round(loop_bars * per_bar);
    if (per_bar < (m_bar_beats * m_config.m_min_tpb) - k_tick_eps ||
        points < static_cast<double>(k_motion_min_points)) {
        return Start::Refused;
    }

    m_bars = bars > 0.0;
    m_points_per_bar = static_cast<size_t>(per_bar);
    m_capacity = static_cast<size_t>(points);
    m_tick_step = loop_bars * m_bar_beats / points;
    m_clock = s.m_beat;
    m_count = 0;
    m_touched = false;
    m_full = false;
    m_last_live = false;
    m_last_x = s.m_x;
    m_last_y = s.m_y;
    m_seam = 0;
    m_active = true;

    if (!m_bars) {
        m_target = m_points_per_bar;
        m_num_points = 0;
        return Start::Started;
    }

    m_length = bars * m_bar_beats;
    m_line = std::floor((s.m_beat / m_length) + k_tick_eps) * m_length;
    m_anchor = wrap_phase(m_line, m_length);
    m_tick_base = m_line;
    m_num_points = m_capacity;
    // The ticks from the loop line to the start: the seed, or lifted at the start position.
    const double ticks = std::ceil(((s.m_beat - m_line) / m_tick_step) - k_tick_eps);
    const size_t lead = std::min(static_cast<size_t>(std::max(ticks, 0.0)), m_capacity - 1);
    const bool seeded = !s.m_seed.empty() && s.m_seed.m_timebase == MotionTimebase::Beats &&
                        s.m_seed.m_x != nullptr && s.m_seed.m_y != nullptr;
    if (seeded) {
        seed(s.m_seed);
        m_full = true;
    } else {
        for (size_t i = 0; i < lead; ++i) { write(i, s.m_x, s.m_y, 0); }
    }
    m_count = lead;
    return Start::Started;
}

// The seed as it plays on this take's grid, from the loop line: one interpolation per point.
void MotionTakeRecorder::seed(const MotionLoopView& s) TANH_NONBLOCKING_FUNCTION {
    const auto n = static_cast<double>(s.m_num_points);
    for (size_t i = 0; i < m_capacity; ++i) {
        const double beat = m_line + (static_cast<double>(i) * m_tick_step);
        const double index = wrap_phase(beat - s.m_anchor, s.m_length) * n / s.m_length;
        const size_t g = std::min(static_cast<size_t>(index), s.m_num_points - 1);
        m_x[i] = catmull_rom_wrap(s.m_x, s.m_num_points, index);
        m_y[i] = catmull_rom_wrap(s.m_y, s.m_num_points, index);
        m_gate[i] = s.m_gate != nullptr ? s.m_gate[g] : 1;
    }
}

void MotionTakeRecorder::write(size_t index, float x, float y, uint8_t gate)
    TANH_NONBLOCKING_FUNCTION {
    m_x[index] = x;
    m_y[index] = y;
    m_gate[index] = gate;
    m_last_x = x;
    m_last_y = y;
}

MotionTakeRecorder::Event MotionTakeRecorder::step(bool live, float x, float y, double slope)
    TANH_NONBLOCKING_FUNCTION {
    if (!m_active) { return Event::None; }
    Event event = Event::None;

    if (!m_bars) {
        if (m_touched && !live) { return finish_free(); }  // the release ends it
        if (!m_touched) {
            if (!live) {
                m_clock += slope;  // waits for the touch
                return Event::None;
            }
            m_touched = true;
            m_tick_base = m_clock;
            m_line = m_clock;
        }
        const double next = m_tick_base + (static_cast<double>(m_count) * m_tick_step);
        if (m_clock >= next - k_tick_eps) {
            write(m_count++, x, y, 1);
            if (m_count >= m_target) {
                if (m_count >= m_capacity) {
                    event = finish_free();
                } else {
                    m_target = std::min(m_target * 2, m_capacity);
                }
            }
        }
        m_clock += slope;
        return event;
    }

    const double next = m_tick_base + (static_cast<double>(m_count) * m_tick_step);
    if (m_clock >= next - k_tick_eps) {
        if (live) {
            write(m_count, x, y, 1);
            m_touched = true;
            m_seam = (m_count + 1) % m_capacity;
        } else if (!m_full) {
            write(m_count, m_last_x, m_last_y, 0);
        }
        if (++m_count >= m_capacity) {  // the loop line: the next pass writes over this one
            m_count = 0;
            m_tick_base += m_length;
            m_full = true;
        }
    }
    if (m_last_live && !live) { event = complete_loop(Event::Revision); }
    m_last_live = live;
    m_clock += slope;
    return event;
}

// A BarsN take as a whole loop: the rest of a first pass is lifted at the last position.
MotionTakeRecorder::Event MotionTakeRecorder::complete_loop(Event event) TANH_NONBLOCKING_FUNCTION {
    if (!m_full) {
        for (size_t i = m_count; i < m_capacity; ++i) { write(i, m_last_x, m_last_y, 0); }
        m_full = true;
    }
    fill_gaps();
    if (event == Event::Finished) { m_active = false; }
    return event;
}

// The loop to hand over: every lifted stretch glides in a straight line from the drawn point
// before it to the drawn point after it, around the loop line. Bounded: one pass over the loop.
void MotionTakeRecorder::fill_gaps() TANH_NONBLOCKING_FUNCTION {
    const size_t n = m_capacity;
    std::copy_n(m_x.begin(), n, m_out_x.begin());
    std::copy_n(m_y.begin(), n, m_out_y.begin());
    std::copy_n(m_gate.begin(), n, m_out_gate.begin());
    size_t first = 0;
    while (first < n && m_gate[first] == 0) { ++first; }
    if (first == n) { return; }  // nothing drawn
    size_t last = first;         // the drawn point before the stretch
    for (size_t k = 1; k <= n; ++k) {
        const size_t i = (first + k) % n;
        if (m_gate[i] == 0) { continue; }
        const size_t span = (i + n - last) % n;  // to the next drawn point (n: the only one)
        const size_t gap = span == 0 ? n : span;
        for (size_t j = 1; j < gap; ++j) {
            const size_t at = (last + j) % n;
            const float w = static_cast<float>(j) / static_cast<float>(gap);
            m_out_x[at] = m_x[last] + ((m_x[i] - m_x[last]) * w);
            m_out_y[at] = m_y[last] + ((m_y[i] - m_y[last]) * w);
            m_out_gate[at] = 1;
        }
        last = i;
    }
}

// A free take loops without a pause: drop the points before the finger first moves and after
// it last moved (each run kept down to one point), then stretch it onto whole bars.
MotionTakeRecorder::Event MotionTakeRecorder::finish_free() TANH_NONBLOCKING_FUNCTION {
    m_active = false;
    size_t n = m_count;
    if (n < k_motion_min_points) { return Event::Dropped; }
    auto distance = [&](size_t i, size_t j) {
        return std::abs(static_cast<double>(m_x[i]) - m_x[j]) +
               std::abs(static_cast<double>(m_y[i]) - m_y[j]);
    };
    size_t first = 0;
    while (first < n && distance(first, 0) <= k_still_distance) { ++first; }
    if (first < n) {  // it moved; a held position is kept as it is
        size_t last = n - 1;
        while (last > 0 && distance(last, n - 1) <= k_still_distance) { --last; }
        const size_t begin = first - 1;
        const size_t end = std::min(n, std::max(last + 2, first + 1));
        if (begin > 0) {
            const auto from = static_cast<std::ptrdiff_t>(begin);
            const auto to = static_cast<std::ptrdiff_t>(end);
            std::copy(m_x.begin() + from, m_x.begin() + to, m_x.begin());
            std::copy(m_y.begin() + from, m_y.begin() + to, m_y.begin());
            std::copy(m_gate.begin() + from, m_gate.begin() + to, m_gate.begin());
        }
        m_line += static_cast<double>(begin) * m_tick_step;
        n = end - begin;
    }
    if (n < k_motion_min_points) { return Event::Dropped; }
    const double beats = static_cast<double>(n) * m_tick_step;
    m_length = snap_bars(beats / m_bar_beats) * m_bar_beats;
    m_anchor = wrap_phase(std::round(m_line / m_bar_beats) * m_bar_beats, m_length);
    m_num_points = n;
    m_seam = 0;
    return Event::Finished;
}

MotionTakeRecorder::Event MotionTakeRecorder::end() TANH_NONBLOCKING_FUNCTION {
    if (!m_active) { return Event::None; }
    if (!m_touched) {
        m_active = false;
        return Event::Cancelled;
    }
    if (!m_bars) { return finish_free(); }
    return complete_loop(Event::Finished);
}

}  // namespace thl::modulation::detail
