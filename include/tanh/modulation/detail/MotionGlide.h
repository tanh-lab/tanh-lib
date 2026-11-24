#pragma once

#include <tanh/core/AbiNamespace.h>
#include <tanh/core/Numbers.h>
#include <tanh/modulation/MotionLane.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace thl::inline THL_ABI_NAMESPACE::modulation::detail {

/// A raised-cosine glide of every channel from where the output was towards a moving target.
class MotionGlide {
public:
    void set_length(uint32_t samples) { m_length = std::max<uint32_t>(samples, 1); }

    /// Glide from @p from over the next length samples.
    void start(const MotionValues& from) TANH_NONBLOCKING_FUNCTION {
        m_left = m_length;
        m_from = from;
    }
    void cancel() TANH_NONBLOCKING_FUNCTION { m_left = 0; }

    /// Moves the first @p num_channels of @p values, this sample's target, to the glide's
    /// value and advances.
    void apply(MotionValues& values, size_t num_channels) TANH_NONBLOCKING_FUNCTION {
        if (m_left == 0) { return; }
        const double w =
            0.5 * (1.0 - std::cos(std::numbers::pi * static_cast<double>(m_length - m_left) /
                                  static_cast<double>(m_length)));
        for (size_t c = 0; c < num_channels; ++c) {
            values[c] = static_cast<float>(m_from[c] + ((values[c] - m_from[c]) * w));
        }
        --m_left;
    }

private:
    uint32_t m_length = 1200;
    uint32_t m_left = 0;
    MotionValues m_from{};
};

}  // namespace thl::inline THL_ABI_NAMESPACE::modulation::detail
