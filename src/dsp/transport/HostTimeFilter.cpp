#include "tanh/dsp/transport/HostTimeFilter.h"

#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace thl::dsp::transport {

void HostTimeFilter::prepare(double sample_rate) TANH_NONBLOCKING_FUNCTION {
    if (sample_rate > 0.0) { m_default_slope = 1e6 / sample_rate; }
    reset();
}

int64_t HostTimeFilter::add(double sample_time, int64_t now_us) TANH_NONBLOCKING_FUNCTION {
    const auto now = static_cast<double>(now_us);
    if (m_count == 0) {
        m_x0 = sample_time;
        m_y0 = now;
    }
    m_x[m_index] = sample_time - m_x0;
    m_y[m_index] = now - m_y0;
    m_index = (m_index + 1) % k_points;
    m_count = std::min(m_count + 1, k_points);

    const auto n = static_cast<double>(m_count);
    double mean_x = 0.0;
    double mean_y = 0.0;
    for (size_t i = 0; i < m_count; ++i) {
        mean_x += m_x[i];
        mean_y += m_y[i];
    }
    mean_x /= n;
    mean_y /= n;
    double sxy = 0.0;
    double sxx = 0.0;
    for (size_t i = 0; i < m_count; ++i) {
        const double dx = m_x[i] - mean_x;
        sxy += dx * (m_y[i] - mean_y);
        sxx += dx * dx;
    }
    const double slope = sxx > 0.0 ? sxy / sxx : m_default_slope;
    const double intercept = mean_y - (slope * mean_x);
    return std::llround(m_y0 + intercept + (slope * (sample_time - m_x0)));
}

}  // namespace thl::dsp::transport
