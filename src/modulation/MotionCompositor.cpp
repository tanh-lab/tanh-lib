#include "tanh/modulation/detail/MotionCompositor.h"

#include <tanh/core/AbiNamespace.h>
#include <tanh/modulation/MotionLane.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace thl::inline THL_ABI_NAMESPACE::modulation::detail {

void MotionCompositor::prepare(double sample_rate,
                               double glide_ms,
                               size_t max_block,
                               size_t num_channels) {
    const double ms = glide_ms >= 0.0 ? glide_ms : 25.0;
    m_glide.set_length(static_cast<uint32_t>(std::max(1.0, std::round(ms * sample_rate / 1000.0))));
    m_glide.cancel();
    m_max_block = max_block;
    m_num_channels = std::clamp<size_t>(num_channels, 1, k_motion_max_channels);
    m_values.assign(m_num_channels * max_block, 0.0f);
    for (size_t c = 0; c < m_num_channels; ++c) {
        std::fill_n(m_values.begin() + static_cast<std::ptrdiff_t>(c * max_block),
                    max_block,
                    m_last[c]);
    }
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
                              const MotionValues& touch,
                              bool playing,
                              const MotionValues& played,
                              uint8_t pg,
                              bool change) TANH_NONBLOCKING_FUNCTION {
    bool cp = change;
    MotionValues out = m_last;
    uint8_t og = 0;
    if (live) {
        // The touch overrides playback, as a gesture overrides automation.
        out = touch;
        og = 1;
        m_glide.cancel();
        m_glide_pending = false;
    } else if (playing) {
        if (m_last_live || m_glide_pending) {
            m_glide.start(m_last);  // back to the running playback
            m_glide_pending = false;
            cp = true;
        }
        out = played;
        m_glide.apply(out, m_num_channels);
        og = pg;
    } else {
        m_glide.cancel();
        m_glide_pending = false;
    }
    if (live != m_last_live || og != m_last_gate) { cp = true; }

    for (size_t c = 0; c < m_num_channels; ++c) { m_values[(c * m_max_block) + i] = out[c]; }
    m_gate[i] = og;
    m_live[i] = live ? 1 : 0;
    if (cp && (m_num_change_points == 0 || m_change_points[m_num_change_points - 1] != i)) {
        m_change_points[m_num_change_points++] = i;
    }
    m_last = out;
    m_last_gate = og;
    m_last_live = live;
}

}  // namespace thl::inline THL_ABI_NAMESPACE::modulation::detail
