#pragma once

#include <tanh/core/Numbers.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace thl::modulation::detail {

/// A raised-cosine glide of x and y from where the output was towards a moving target.
class MotionGlide {
public:
    void set_length(uint32_t samples) { m_length = std::max<uint32_t>(samples, 1); }

    /// Glide from (@p x, @p y) over the next length samples.
    void start(float x, float y) TANH_NONBLOCKING_FUNCTION {
        m_left = m_length;
        m_from_x = x;
        m_from_y = y;
    }
    void cancel() TANH_NONBLOCKING_FUNCTION { m_left = 0; }

    /// Moves (@p x, @p y), this sample's target, to the glide's value and advances.
    void apply(float& x, float& y) TANH_NONBLOCKING_FUNCTION {
        if (m_left == 0) { return; }
        const double w =
            0.5 * (1.0 - std::cos(std::numbers::pi * static_cast<double>(m_length - m_left) /
                                  static_cast<double>(m_length)));
        x = static_cast<float>(m_from_x + ((x - m_from_x) * w));
        y = static_cast<float>(m_from_y + ((y - m_from_y) * w));
        --m_left;
    }

private:
    uint32_t m_length = 1200;
    uint32_t m_left = 0;
    float m_from_x = 0.0f;
    float m_from_y = 0.0f;
};

}  // namespace thl::modulation::detail
