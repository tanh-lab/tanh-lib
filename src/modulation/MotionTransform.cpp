#include "tanh/modulation/MotionTransform.h"

#include <tanh/core/AbiNamespace.h>
#include <tanh/modulation/MotionLane.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace thl::inline THL_ABI_NAMESPACE::modulation {

MotionLane render_motion_path(const MotionLane& lane,
                              const MotionTransform* transform,
                              uint64_t settings,
                              size_t num_points,
                              size_t num_channels) {
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
    const size_t channels =
        std::min(take ? lane.num_channels() : num_channels, k_motion_max_channels);
    path.m_channels.assign(channels, std::vector<float>(num_points));
    path.m_gate.resize(num_points);
    for (size_t i = 0; i < num_points; ++i) {
        const double u = static_cast<double>(i) / static_cast<double>(num_points);
        MotionPoint p;
        p.m_values.fill(0.5f);
        p.m_gate = 1;
        if (take) {
            p = lane.sample(u * lane.m_length);
            if (info.m_opens_gate) { p.m_gate = 1; }
        }
        transform->place(settings, u, std::span<float>(p.m_values.data(), channels));
        for (size_t c = 0; c < channels; ++c) { path.m_channels[c][i] = p.m_values[c]; }
        path.m_gate[i] = p.m_gate;
    }
    return path;
}

}  // namespace thl::inline THL_ABI_NAMESPACE::modulation
