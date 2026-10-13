#include "tanh/modulation/detail/MotionCompositor.h"

#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace thl::modulation::detail {

void MotionCompositor::prepare(double sample_rate, double glide_ms, size_t max_block) {
    const double ms = glide_ms >= 0.0 ? glide_ms : 25.0;
    m_glide.set_length(static_cast<uint32_t>(std::max(1.0, std::round(ms * sample_rate / 1000.0))));
    m_glide.cancel();
    m_x.assign(max_block, m_last_x);
    m_y.assign(max_block, m_last_y);
    m_gate.assign(max_block, 0);
    m_live.assign(max_block, 0);
    m_change_points.assign(max_block + 1, 0);
    m_num_change_points = 0;
    m_glide_pending = false;
}

void MotionCompositor::begin(bool glide) TANH_NONBLOCKING_FUNCTION {
    m_num_change_points = 0;
    m_glide_pending = glide;
}

void MotionCompositor::sample(uint32_t i,
                              bool live,
                              float lx,
                              float ly,
                              bool playing,
                              float px,
                              float py,
                              uint8_t pg,
                              bool change) TANH_NONBLOCKING_FUNCTION {
    bool cp = change;
    float ox = m_last_x;
    float oy = m_last_y;
    uint8_t og = 0;
    if (live) {
        // The touch overrides playback, as a gesture overrides automation.
        ox = lx;
        oy = ly;
        og = 1;
        m_glide.cancel();
        m_glide_pending = false;
    } else if (playing) {
        if (m_last_live || m_glide_pending) {
            m_glide.start(m_last_x, m_last_y);  // back to the running playback
            m_glide_pending = false;
            cp = true;
        }
        ox = px;
        oy = py;
        m_glide.apply(ox, oy);
        og = pg;
    } else {
        m_glide.cancel();
        m_glide_pending = false;
    }
    if (live != m_last_live || og != m_last_gate) { cp = true; }

    m_x[i] = ox;
    m_y[i] = oy;
    m_gate[i] = og;
    m_live[i] = live ? 1 : 0;
    if (cp && (m_num_change_points == 0 || m_change_points[m_num_change_points - 1] != i)) {
        m_change_points[m_num_change_points++] = i;
    }
    m_last_x = ox;
    m_last_y = oy;
    m_last_gate = og;
    m_last_live = live;
}

}  // namespace thl::modulation::detail
