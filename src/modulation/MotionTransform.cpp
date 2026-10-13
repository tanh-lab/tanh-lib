#include "tanh/modulation/MotionTransform.h"

#include <tanh/modulation/MotionLane.h>

#include <cstddef>
#include <cstdint>

namespace thl::modulation {

MotionLane render_motion_path(const MotionLane& lane,
                              const MotionTransform* transform,
                              uint64_t settings,
                              size_t num_points) {
    if (transform == nullptr || num_points == 0) { return lane; }
    const MotionTransformInfo info = transform->info(settings);
    if (!info.m_active) { return lane; }
    const bool take = info.m_uses_lane && !lane.empty();
    if (!take && !info.m_plays_alone) { return info.m_uses_lane ? MotionLane{} : lane; }
    MotionLane path;
    path.m_take_id = lane.m_take_id;
    path.m_timebase = take ? lane.m_timebase : MotionTimebase::Beats;
    path.m_length = take ? lane.m_length : 4.0;
    path.m_anchor = take ? lane.m_anchor : 0.0;
    path.m_rate = static_cast<double>(num_points) / path.m_length;
    path.m_x.resize(num_points);
    path.m_y.resize(num_points);
    path.m_gate.resize(num_points);
    for (size_t i = 0; i < num_points; ++i) {
        const double u = static_cast<double>(i) / static_cast<double>(num_points);
        float x = 0.5f;
        float y = 0.5f;
        uint8_t gate = 1;
        if (take) {
            const MotionPoint p = lane.sample(u * lane.m_length);
            x = p.m_x;
            y = p.m_y;
            gate = info.m_opens_gate ? 1 : p.m_gate;
        }
        transform->place(settings, u, x, y);
        path.m_x[i] = x;
        path.m_y[i] = y;
        path.m_gate[i] = gate;
    }
    return path;
}

}  // namespace thl::modulation
